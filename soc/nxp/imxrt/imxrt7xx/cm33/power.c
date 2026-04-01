/*
 * Copyright 2025 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/pm/pm.h>
#include <zephyr/logging/log.h>
#include "fsl_clock.h"
#include "fsl_power.h"

LOG_MODULE_REGISTER(soc_power, CONFIG_SOC_LOG_LEVEL);

#define DEEP_SLEEP_CONFIG		\
	((const uint32_t[]) DT_PROP_OR(DT_NODELABEL(deepsleep), deep_sleep_config, {}))

#if defined(CONFIG_SOC_MIMXRT798S_CM33_CPU0)
#define DSR_CONFIG ((const uint32_t[])	\
	DT_PROP_OR(DT_NODELABEL(deepsleepretention), deep_sleep_retention_config, {}))
#endif

static inline void imxrt7xx_prepare_power_state(enum pm_state state)
{
	ARG_UNUSED(state);
}

static inline void imxrt7xx_resume_power_state(enum pm_state state)
{
	ARG_UNUSED(state);
}

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	/*
	 * The kernel reaches this hook with BASEPRI masking enabled. Switch to
	 * PRIMASK-based masking so wakeup interrupts can bring the CPU out of deep
	 * sleep.
	 */
	__disable_irq();
	irq_unlock(0);

	switch (state) {
	case PM_STATE_RUNTIME_IDLE:
		LOG_DBG("Enter sleep");
		POWER_EnterSleep();
		break;
	case PM_STATE_SUSPEND_TO_IDLE:
		LOG_DBG("Enter deep sleep");
		imxrt7xx_prepare_power_state(state);
		POWER_EnterDeepSleep(DEEP_SLEEP_CONFIG);
		imxrt7xx_resume_power_state(state);
		break;
#if defined(CONFIG_SOC_MIMXRT798S_CM33_CPU0)
	case PM_STATE_STANDBY:
		LOG_DBG("Enter DSR");
		imxrt7xx_prepare_power_state(state);
		POWER_EnterDSR(DSR_CONFIG);
		imxrt7xx_resume_power_state(state);
		break;
#endif
	default:
		LOG_WRN("Unsupported power state %u", state);
		__enable_irq();
	}
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(state);
	ARG_UNUSED(substate_id);

	__enable_irq();
}
