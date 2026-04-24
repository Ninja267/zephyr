/*
 * Copyright (c) 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Subsystem-layer implementation of clock_monitor_measure(). Wraps the
 * back-end's async .measure vtable op with a sem-based wait and
 * timeout.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/clock_monitor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(clock_monitor_measure, CONFIG_CLOCK_MONITOR_LOG_LEVEL);

struct z_clock_monitor_measure_cb_data {
	struct k_sem sem;
	uint32_t rate_hz;
	int status;
};

static void z_clock_monitor_measure_callback(const struct device *dev,
					     const struct clock_monitor_event_data *evt,
					     void *user_data)
{
	struct z_clock_monitor_measure_cb_data *data = user_data;

	ARG_UNUSED(dev);

	if (evt->events & CLOCK_MONITOR_EVT_MEASURE_DONE) {
		data->status  = 0;
		data->rate_hz = evt->measured_hz;
	} else if (evt->events & CLOCK_MONITOR_EVT_CLOCK_LOST) {
		data->status = -EIO;
	} else {
		/* Otherwise leave status at its initial -EAGAIN; the wrapper below
		 * still gets unblocked and returns whatever the cb decided.
		 */
	}
	k_sem_give(&data->sem);
}

int z_impl_clock_monitor_measure(const struct device *dev, uint32_t *rate_hz,
				 k_timeout_t timeout)
{
	const struct clock_monitor_driver_api *api =
		DEVICE_API_GET(clock_monitor, dev);
	struct z_clock_monitor_measure_cb_data data;
	int err;

	if (api->measure == NULL) {
		return -ENOSYS;
	}

	k_sem_init(&data.sem, 0, 1);
	data.status  = -EAGAIN;
	data.rate_hz = 0U;

	err = api->measure(dev, z_clock_monitor_measure_callback, &data);
	if (err != 0) {
		return err;
	}

	if (k_sem_take(&data.sem, timeout) != 0) {
		if (api->stop != NULL) {
			(void)api->stop(dev);
		}
		LOG_WRN("%s: measure timed out", dev->name);
		return -EAGAIN;
	}

	if (data.status == 0 && rate_hz != NULL) {
		*rate_hz = data.rate_hz;
	}
	return data.status;
}
