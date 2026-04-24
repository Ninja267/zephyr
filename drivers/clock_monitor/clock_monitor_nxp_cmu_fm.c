/*
 * Copyright (c) 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr clock_monitor back-end for NXP CMU_FM (Frequency Meter).
 *
 */

#define DT_DRV_COMPAT nxp_cmu_fm

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_monitor.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <fsl_cmu_fm.h>

#include "clock_monitor_common.h"

LOG_MODULE_REGISTER(clock_monitor_nxp_cmu_fm,
		    CONFIG_CLOCK_MONITOR_LOG_LEVEL);

struct nxp_cmu_fm_config {
	CMU_FM_Type *base;
	const struct device    *ref_clk_dev;
	clock_control_subsys_t  ref_clk_subsys;
	const struct device    *mon_clk_dev;
	clock_control_subsys_t  mon_clk_subsys;
	void (*irq_config_func)(const struct device *dev);
};

enum nxp_cmu_fm_state {
	NXP_CMU_FM_STATE_IDLE = 0,
	NXP_CMU_FM_STATE_CONFIGURED,
	NXP_CMU_FM_STATE_RUNNING,
};

struct nxp_cmu_fm_data {
	/* Provides exclusion against multiple concurrent requests */
	struct k_mutex op_mutex;
	/* Serializes between ISR and other calls */
	struct k_spinlock lock;
	enum nxp_cmu_fm_state state;
	struct clock_monitor_config cfg;
	uint32_t ref_hz;            /* cached from clock_control at configure */
	uint32_t ref_cnt;
	uint32_t latched_events;
	clock_monitor_callback_t async_cb;
	void *async_user_data;
};

static int nxp_cmu_fm_configure(const struct device *dev,
				const struct clock_monitor_config *cfg)
{
	const struct nxp_cmu_fm_config *config = dev->config;
	struct nxp_cmu_fm_data *data = dev->data;
	k_spinlock_key_t key;
	enum nxp_cmu_fm_state state;
	int ret;

	/* Input-only validation, no state needed. */
	if (cfg->mode != CLOCK_MONITOR_MODE_MEASURE &&
	    cfg->mode != CLOCK_MONITOR_MODE_DISABLED) {
		return -ENOTSUP;
	}

	if (cfg->mode == CLOCK_MONITOR_MODE_MEASURE &&
	    cfg->measure.window_ns == 0U) {
		return -EINVAL;
	}

	k_mutex_lock(&data->op_mutex, K_FOREVER);

	key = k_spin_lock(&data->lock);
	state = data->state;
	k_spin_unlock(&data->lock, key);

	if (state == NXP_CMU_FM_STATE_RUNNING) {
		k_mutex_unlock(&data->op_mutex);
		return -EBUSY;
	}

	if (cfg->mode == CLOCK_MONITOR_MODE_DISABLED) {
		if (state != NXP_CMU_FM_STATE_IDLE) {
			CMU_FM_Deinit(config->base);
		}

		key = k_spin_lock(&data->lock);
		data->cfg = *cfg;
		data->ref_hz = 0U;
		data->ref_cnt = 0U;
		data->latched_events = 0U;
		data->async_cb = NULL;
		data->async_user_data = NULL;
		data->state = NXP_CMU_FM_STATE_IDLE;
		k_spin_unlock(&data->lock, key);
		k_mutex_unlock(&data->op_mutex);
		return 0;
	}

	/* Query reference clock rate from clock_control for this configure. */
	uint32_t ref_hz = 0U;
	int cc_ret = clock_control_get_rate(config->ref_clk_dev,
					    config->ref_clk_subsys, &ref_hz);

	if (cc_ret != 0 || ref_hz == 0U) {
		k_mutex_unlock(&data->op_mutex);
		return -EIO;
	}

	uint32_t ref_cnt;

	ret = clock_monitor_compute_ref_cnt(cfg->measure.window_ns, ref_hz,
					    CMU_FM_RCCR_REF_CNT_MASK, &ref_cnt);
	if (ret != 0) {
		LOG_ERR("ref_cnt out of range: window_ns=%u, ref_hz=%u "
			"(HW max %u)", cfg->measure.window_ns, ref_hz,
			(uint32_t)CMU_FM_RCCR_REF_CNT_MASK);
		k_mutex_unlock(&data->op_mutex);
		return ret;
	}

	cmu_fm_config_t hal_cfg;

	CMU_FM_GetDefaultConfig(&hal_cfg);
	hal_cfg.refClockCount = ref_cnt;
	hal_cfg.enableInterrupt = (config->irq_config_func != NULL);

	status_t st = CMU_FM_Init(config->base, &hal_cfg);

	if (st != kStatus_Success) {
		key = k_spin_lock(&data->lock);
		data->state = NXP_CMU_FM_STATE_IDLE;
		k_spin_unlock(&data->lock, key);
		k_mutex_unlock(&data->op_mutex);
		return -EIO;
	}

	key = k_spin_lock(&data->lock);
	data->cfg = *cfg;
	data->latched_events = 0U;
	data->ref_hz = ref_hz;
	data->ref_cnt = ref_cnt;
	data->async_cb = NULL;
	data->async_user_data = NULL;
	data->state = NXP_CMU_FM_STATE_CONFIGURED;
	k_spin_unlock(&data->lock, key);
	k_mutex_unlock(&data->op_mutex);
	return 0;
}

static int nxp_cmu_fm_stop(const struct device *dev)
{
	const struct nxp_cmu_fm_config *config = dev->config;
	struct nxp_cmu_fm_data *data = dev->data;
	k_spinlock_key_t key;

	k_mutex_lock(&data->op_mutex, K_FOREVER);
	key = k_spin_lock(&data->lock);

	if (data->state == NXP_CMU_FM_STATE_RUNNING) {
		CMU_FM_StopFreqMetering(config->base);
		data->state = NXP_CMU_FM_STATE_CONFIGURED;
	}
	data->async_cb = NULL;
	data->async_user_data = NULL;

	k_spin_unlock(&data->lock, key);
	k_mutex_unlock(&data->op_mutex);
	return 0;
}

static int nxp_cmu_fm_measure(const struct device *dev,
			      clock_monitor_callback_t cb,
			      void *user_data)
{
	const struct nxp_cmu_fm_config *config = dev->config;
	struct nxp_cmu_fm_data *data = dev->data;
	k_spinlock_key_t key;

	if (config->irq_config_func == NULL) {
		return -ENOSYS;
	}

	k_mutex_lock(&data->op_mutex, K_FOREVER);

	key = k_spin_lock(&data->lock);
	if (data->state != NXP_CMU_FM_STATE_CONFIGURED ||
	    data->cfg.mode != CLOCK_MONITOR_MODE_MEASURE) {
		k_spin_unlock(&data->lock, key);
		k_mutex_unlock(&data->op_mutex);
		return -EINVAL;
	}

	data->async_cb        = cb;
	data->async_user_data = user_data;
	data->state = NXP_CMU_FM_STATE_RUNNING;
	k_spin_unlock(&data->lock, key);

	CMU_FM_StartFreqMetering(config->base);

	k_mutex_unlock(&data->op_mutex);
	return 0;
}

static int nxp_cmu_fm_get_events(const struct device *dev, uint32_t *events)
{
	struct nxp_cmu_fm_data *data = dev->data;

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	*events = data->latched_events;
	data->latched_events = 0U;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static void nxp_cmu_fm_isr(const struct device *dev)
{
	const struct nxp_cmu_fm_config *config = dev->config;
	struct nxp_cmu_fm_data *data = dev->data;
	uint32_t flags = CMU_FM_GetStatusFlags(config->base);
	uint32_t evts = 0U;
	uint32_t meas_rate_hz = 0U;
	bool meas_done = false;
	clock_monitor_callback_t async_cb = NULL;
	void *async_user_data = NULL;

	CMU_FM_ClearStatusFlags(config->base, flags);

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if ((flags & (uint32_t)kCMU_FM_MeterComplete) != 0U) {
		uint32_t met_cnt = CMU_FM_GetMeteredClkCnt(config->base);

		meas_rate_hz = CMU_FM_CalcMeteredClkFreq(
			met_cnt, data->ref_cnt, data->ref_hz);
		evts |= CLOCK_MONITOR_EVT_MEASURE_DONE;
		meas_done = true;
		data->state = NXP_CMU_FM_STATE_CONFIGURED;
	}

	if ((flags & (uint32_t)kCMU_FM_MeterTimeout) != 0U) {
		evts |= CLOCK_MONITOR_EVT_CLOCK_LOST;
		data->state = NXP_CMU_FM_STATE_CONFIGURED;
		meas_done = true;
	}

	if (meas_done) {
		async_cb        = data->async_cb;
		async_user_data = data->async_user_data;
		data->async_cb        = NULL;
		data->async_user_data = NULL;
	}

	data->latched_events |= evts;

	k_spin_unlock(&data->lock, key);

	if (async_cb != NULL) {
		struct clock_monitor_event_data async_evt = {
			.events      = evts,
			.measured_hz = meas_rate_hz,
		};
		async_cb(dev, &async_evt, async_user_data);
	}
}

static int nxp_cmu_fm_init(const struct device *dev)
{
	const struct nxp_cmu_fm_config *config = dev->config;
	struct nxp_cmu_fm_data *data = dev->data;

	k_mutex_init(&data->op_mutex);
	data->state = NXP_CMU_FM_STATE_IDLE;

	if (config->irq_config_func != NULL) {
		config->irq_config_func(dev);
	} else {
		LOG_INF("%s: instantiated without NVIC IRQ (polling-only)",
			dev->name);
	}
	return 0;
}

static DEVICE_API(clock_monitor, nxp_cmu_fm_api) = {
	.configure  = nxp_cmu_fm_configure,
	.stop       = nxp_cmu_fm_stop,
	.measure    = nxp_cmu_fm_measure,
	.get_events = nxp_cmu_fm_get_events,
};

/* ---------- Instantiation ---------- */

#define NXP_CMU_FM_IRQ_WIRE(inst)                                              \
	static void nxp_cmu_fm_irq_cfg_##inst(const struct device *dev)        \
	{                                                                      \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority),   \
			    nxp_cmu_fm_isr, DEVICE_DT_INST_GET(inst), 0);      \
		irq_enable(DT_INST_IRQN(inst));                                \
	}

#define NXP_CMU_FM_IRQ_PTR(inst)                                               \
	COND_CODE_1(DT_INST_IRQ_HAS_IDX(inst, 0),                              \
		    (nxp_cmu_fm_irq_cfg_##inst), (NULL))

#define NXP_CMU_FM_DEVICE_INIT(inst)                                           \
	COND_CODE_1(DT_INST_IRQ_HAS_IDX(inst, 0),                              \
		    (NXP_CMU_FM_IRQ_WIRE(inst)), ())                           \
	static struct nxp_cmu_fm_data nxp_cmu_fm_data_##inst;                  \
	static const struct nxp_cmu_fm_config nxp_cmu_fm_cfg_##inst = {        \
		.base = (CMU_FM_Type *)DT_INST_REG_ADDR(inst),                 \
		.ref_clk_dev = DEVICE_DT_GET(                                  \
			DT_INST_CLOCKS_CTLR_BY_NAME(inst, reference)),         \
		.ref_clk_subsys = (clock_control_subsys_t)(uintptr_t)          \
			DT_INST_CLOCKS_CELL_BY_NAME(inst, reference, name),    \
		.mon_clk_dev = DEVICE_DT_GET(                                  \
			DT_INST_CLOCKS_CTLR_BY_NAME(inst, monitored)),         \
		.mon_clk_subsys = (clock_control_subsys_t)(uintptr_t)          \
			DT_INST_CLOCKS_CELL_BY_NAME(inst, monitored, name),    \
		.irq_config_func = NXP_CMU_FM_IRQ_PTR(inst),                   \
	};                                                                     \
	DEVICE_DT_INST_DEFINE(inst, nxp_cmu_fm_init, NULL,                     \
			      &nxp_cmu_fm_data_##inst,                         \
			      &nxp_cmu_fm_cfg_##inst,                          \
			      POST_KERNEL,                                     \
			      CONFIG_CLOCK_MONITOR_INIT_PRIORITY,              \
			      &nxp_cmu_fm_api);

DT_INST_FOREACH_STATUS_OKAY(NXP_CMU_FM_DEVICE_INIT)
