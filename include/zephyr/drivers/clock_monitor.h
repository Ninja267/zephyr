/*
 * Copyright (c) 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public Clock Monitor driver API.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_CLOCK_MONITOR_H_
#define ZEPHYR_INCLUDE_DRIVERS_CLOCK_MONITOR_H_

/**
 * @brief Clock Monitor Interface
 * @defgroup clock_monitor_interface Clock Monitor
 * @since 4.5
 * @version 0.1.0
 * @ingroup io_interfaces
 * @{
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Modes ---------- */

/** @brief Operating mode of a clock monitor instance. */
enum clock_monitor_mode {
	/** Monitor disabled / idle. */
	CLOCK_MONITOR_MODE_DISABLED = 0,
	/** Continuous high/low threshold window check: the frequency of the
	 *  monitored clock is continuously compared against programmable
	 *  upper and lower bounds and events are raised when it leaves the
	 *  window.
	 */
	CLOCK_MONITOR_MODE_WINDOW,
	/** Single-shot frequency measurement reporting the measured value
	 *  in Hz. Periodic sampling is the application's responsibility:
	 *  call @ref clock_monitor_measure in a loop with whatever cadence
	 *  is needed.
	 */
	CLOCK_MONITOR_MODE_MEASURE,
};

/* ---------- Events (bitmask) ---------- */

/** @brief Clock monitor event flags, OR-combined into `uint32_t` masks. */
enum {
	/** Monitored frequency exceeded the upper threshold. */
	CLOCK_MONITOR_EVT_FREQ_HIGH    = BIT(0),
	/** Monitored frequency fell below the lower threshold. */
	CLOCK_MONITOR_EVT_FREQ_LOW     = BIT(1),
	/** Monitored clock stopped or stuck. */
	CLOCK_MONITOR_EVT_CLOCK_LOST   = BIT(2),
	/** Single-shot frequency measurement completed. */
	CLOCK_MONITOR_EVT_MEASURE_DONE = BIT(3),
};

/* ---------- Per-mode configurations ---------- */

/** @brief Configuration for @ref CLOCK_MONITOR_MODE_WINDOW. */
struct clock_monitor_window_cfg {
	/** Nominal expected frequency of the monitored clock (Hz). */
	uint32_t expected_hz;
	/** Acceptable deviation in parts-per-million (± value). */
	uint32_t tolerance_ppm;
	/** Measurement window duration in ns. Must be > 0. */
	uint32_t window_ns;
};

/** @brief Configuration for @ref CLOCK_MONITOR_MODE_MEASURE. */
struct clock_monitor_measure_cfg {
	/** Measurement window duration in ns. Must be > 0. */
	uint32_t window_ns;
};

/* ---------- Event data & callback ---------- */

/** @brief Event payload delivered to the user callback. */
struct clock_monitor_event_data {
	/** Bitmask of latched `CLOCK_MONITOR_EVT_*` flags. */
	uint32_t events;
	/** Last measured frequency (Hz); valid when
	 *  `events & CLOCK_MONITOR_EVT_MEASURE_DONE`.
	 */
	uint32_t measured_hz;
};

/**
 * @brief Callback invoked on clock monitor events.
 *
 * May be called in ISR context. Keep the callback short; defer heavy work
 * to a thread / work queue.
 *
 * @param dev       Clock monitor device.
 * @param evt       Event payload. Lifetime ends when the callback returns.
 * @param user_data Opaque pointer copied from `clock_monitor_config`.
 */
typedef void (*clock_monitor_callback_t)(const struct device *dev,
					 const struct clock_monitor_event_data *evt,
					 void *user_data);

/**
 * @brief Top-level configuration handed to @ref clock_monitor_configure.
 *
 * The callback (plus its user_data) is installed atomically with the rest
 * of the configuration. Pass `callback = NULL` to disable async callback
 * delivery; events are still latched and observable via @ref
 * clock_monitor_get_events.
 */
struct clock_monitor_config {
	/** Operating mode to activate. */
	enum clock_monitor_mode mode;
	/** Per-mode parameters; selected by @p mode. */
	union {
		/** Honored when mode is WINDOW. */
		struct clock_monitor_window_cfg window;
		/** Honored when mode is MEASURE. */
		struct clock_monitor_measure_cfg measure;
	};
	/** Optional callback for asynchronous event delivery. */
	clock_monitor_callback_t callback;
	/** Opaque user pointer passed to @p callback. */
	void *user_data;
};

/* ---------- API function typedefs ---------- */

/** @brief Callback API for configuring a clock monitor. */
typedef int (*clock_monitor_api_configure)(const struct device *dev,
					   const struct clock_monitor_config *cfg);

/** @brief Callback API for starting continuous frequency monitoring. */
typedef int (*clock_monitor_api_start)(const struct device *dev);

/** @brief Callback API for stopping the clock monitor. */
typedef int (*clock_monitor_api_stop)(const struct device *dev);

/**
 * @brief Callback API for kicking off a one-shot measurement and
 *        delivering the result via @p cb.
 *
 * The measurement result is delivered later through @p cb. How the
 * back-end detects completion is an implementation detail: a hardware
 * completion interrupt where available, or an internal timer / work
 * queue that polls a status bit on hardware that lacks such an
 * interrupt. In either case the caller's thread is not blocked inside
 * this call.
 *
 * The completion is delivered through @p cb (not the @ref
 * clock_monitor_config callback installed at configure time). On success
 * `evt->events` carries @ref CLOCK_MONITOR_EVT_MEASURE_DONE and
 * `evt->measured_hz` holds the measured frequency in Hz. If the
 * hardware reports that the monitored clock did not produce enough
 * edges within the measurement window, `evt->events` carries
 * @ref CLOCK_MONITOR_EVT_CLOCK_LOST and `evt->measured_hz` is 0.
 * Both flags may be set if the hardware latches both conditions.
 */

typedef int (*clock_monitor_api_measure)(const struct device *dev,
					 clock_monitor_callback_t cb,
					 void *user_data);

/** @brief Callback API for reading and clearing latched event flags. */
typedef int (*clock_monitor_api_get_events)(const struct device *dev,
					    uint32_t *events);

/**
 * @brief Callback API for switching the reference / target clock inputs.
 *
 */
typedef int (*clock_monitor_api_set_source)(const struct device *dev,
					    uint32_t reference,
					    uint32_t target);

/** @brief Back-end driver vtable (6 entries). */
__subsystem struct clock_monitor_driver_api {
	/** @copybrief clock_monitor_configure */
	clock_monitor_api_configure      configure;
	/** @copybrief clock_monitor_start */
	clock_monitor_api_start          start;
	/** @copybrief clock_monitor_stop */
	clock_monitor_api_stop           stop;
	/** @copybrief clock_monitor_measure */
	clock_monitor_api_measure        measure;
	/** @copybrief clock_monitor_get_events */
	clock_monitor_api_get_events     get_events;
	/** @copybrief clock_monitor_set_source */
	clock_monitor_api_set_source     set_source;
};

/* ---------- Syscalls + inline wrappers ---------- */

/**
 * @brief Apply a monitor configuration.
 *
 * Must be called when the monitor is stopped. Installs `cfg->callback`
 * (may be NULL) atomically with the rest of the configuration. Switching
 * mode implicitly stops any ongoing measurement.
 *
 * @retval 0        success
 * @retval -EINVAL  malformed configuration
 * @retval -ENOTSUP back-end does not support the requested mode
 * @retval -EBUSY   monitor is running
 * @retval -ENOSYS  back-end does not implement configure()
 */
__syscall int clock_monitor_configure(const struct device *dev,
				      const struct clock_monitor_config *cfg);

static inline int z_impl_clock_monitor_configure(const struct device *dev,
						 const struct clock_monitor_config *cfg)
{
	const struct clock_monitor_driver_api *api =
		DEVICE_API_GET(clock_monitor, dev);

	if (api->configure == NULL) {
		return -ENOSYS;
	}
	return api->configure(dev, cfg);
}

/**
 * @brief Start continuous frequency monitoring.
 *
 * Valid in @ref CLOCK_MONITOR_MODE_WINDOW. The device must already have
 * been configured via @ref clock_monitor_configure. For MEASURE mode use
 * @ref clock_monitor_measure instead.
 */
__syscall int clock_monitor_start(const struct device *dev);

static inline int z_impl_clock_monitor_start(const struct device *dev)
{
	const struct clock_monitor_driver_api *api =
		DEVICE_API_GET(clock_monitor, dev);

	if (api->start == NULL) {
		return -ENOSYS;
	}
	return api->start(dev);
}

/** @brief Stop the monitor. */
__syscall int clock_monitor_stop(const struct device *dev);

static inline int z_impl_clock_monitor_stop(const struct device *dev)
{
	const struct clock_monitor_driver_api *api =
		DEVICE_API_GET(clock_monitor, dev);

	if (api->stop == NULL) {
		return -ENOSYS;
	}
	return api->stop(dev);
}

/**
 * @brief Single-shot frequency measurement; calling thread sleeps until
 *        the measurement completes.
 *
 * The device must already be configured in @ref CLOCK_MONITOR_MODE_MEASURE
 * via @ref clock_monitor_configure. This call triggers one measurement
 * and waits for its completion event. The calling thread is suspended
 * (not busy-waiting) until the back-end signals completion. Back-ends
 * with a hardware measurement-completion interrupt should use that
 * interrupt; back-ends whose hardware lacks one MAY internally poll a
 * status bit from a timer or work-queue context. In either case the
 * polling cadence and completion mechanism are back-end implementation
 * details that do not affect this call's contract.
 *
 * Periodic sampling is the application's responsibility: call this
 * function in a loop with whatever cadence (e.g. `k_sleep` between
 * iterations) the use case requires. The driver does not auto-rearm
 * the hardware between measurements.
 *
 * @param dev      Device, configured in MEASURE mode.
 * @param rate_hz  Out: measured frequency in Hz.
 * @param timeout  Max wait; `K_FOREVER` allowed.
 *
 * @retval 0       measurement complete
 * @retval -EINVAL device not configured in MEASURE mode
 * @retval -EAGAIN timeout elapsed before completion
 * @retval -EIO    monitored clock lost before the measurement completed
 * @retval -ENOSYS back-end does not implement measure()
 */
__syscall int clock_monitor_measure(const struct device *dev,
				    uint32_t *rate_hz,
				    k_timeout_t timeout);

/**
 * @brief Read and clear the latched event flags.
 *
 * Returns the set of events that have occurred since the previous call
 * (or since the monitor was configured) and atomically clears them, so
 * the same events will not be reported again.
 *
 * Intended for polling-mode usage and for draining events that occurred
 * while no callback was installed.
 *
 * @param dev    Device.
 * @param events Out: bitmask of `CLOCK_MONITOR_EVT_*` flags.
 *
 * @retval 0       success
 * @retval -ENOSYS unsupported
 */
__syscall int clock_monitor_get_events(const struct device *dev,
				       uint32_t *events);

static inline int z_impl_clock_monitor_get_events(const struct device *dev,
						  uint32_t *events)
{
	const struct clock_monitor_driver_api *api =
		DEVICE_API_GET(clock_monitor, dev);

	if (api->get_events == NULL) {
		return -ENOSYS;
	}
	return api->get_events(dev, events);
}

/**
 * @brief Switch the reference / target clock inputs at runtime.
 *
 * The @p reference and @p target arguments are opaque back-end cookies.
 * Each back-end exposes its accepted encodings through a dt-bindings
 * header (e.g. `<zephyr/dt-bindings/clock_monitor/<vendor>-<ip>.h>`)
 * so that devicetree properties and runtime calls share a single set
 * of constants.
 *
 *
 * @param dev        Clock monitor device.
 * @param reference  Reference clock cookie, or sentinel.
 * @param target     Target clock cookie, or sentinel.
 *
 * @retval 0        success
 * @retval -ENOTSUP back-end's hardware cannot switch sources at runtime
 * @retval -EINVAL  one or both cookies are unknown to this back-end
 * @retval -EBUSY   monitor running or measurement in progress
 * @retval -ENOSYS  back-end does not implement set_source()
 */
__syscall int clock_monitor_set_source(const struct device *dev,
				       uint32_t reference, uint32_t target);

static inline int z_impl_clock_monitor_set_source(const struct device *dev,
						  uint32_t reference,
						  uint32_t target)
{
	const struct clock_monitor_driver_api *api =
		DEVICE_API_GET(clock_monitor, dev);

	if (api->set_source == NULL) {
		return -ENOSYS;
	}
	return api->set_source(dev, reference, target);
}

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#include <zephyr/syscalls/clock_monitor.h>

#endif /* ZEPHYR_INCLUDE_DRIVERS_CLOCK_MONITOR_H_ */
