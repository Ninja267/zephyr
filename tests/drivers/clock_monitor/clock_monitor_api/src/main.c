/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Vendor-neutral clock_monitor API tests.
 *
 * The board overlay lists all clock_monitor instances to exercise under
 *   /zephyr,user { clock-monitors = <&dev0> [, <&dev1> ...]; };
 *
 * Each case iterates the array and, for every device that accepts the
 * mode the case needs (configure() does not return -ENOTSUP), runs the
 * body. A case that finds no matching device ztest_test_skip()'s so the
 * suite is portable across back-ends that only support a subset of
 * modes.
 *
 * A single device that supports both WINDOW and MEASURE (e.g. Renesas
 * CAC) is listed once; mode-probing makes every case run on it. A
 * multi-device board (e.g. NXP MCXE31 with cmu_0 FC + cmu_1 FM) lists
 * both and each case runs on whichever device supports it.
 */

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_monitor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#define CLKMON_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(CLKMON_NODE, clock_monitors)

#define TO_DEV(node_id, prop, idx) \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

static const struct device *const clock_devices[] = {
	DT_FOREACH_PROP_ELEM(CLKMON_NODE, clock_monitors, TO_DEV)
};

#define NUM_CLOCK_DEVICES ARRAY_SIZE(clock_devices)

#else

static const struct device *const clock_devices[] = {NULL};

#define NUM_CLOCK_DEVICES 0U

#endif

/* ------------------------------------------------------------------ */
/* Shared configurations                                               */
/* ------------------------------------------------------------------ */

static const struct clock_monitor_config window_cfg = {
	.mode = CLOCK_MONITOR_MODE_WINDOW,
	.window = {
		.expected_hz   = CONFIG_TEST_CLOCK_MONITOR_EXPECTED_HZ,
		.tolerance_ppm = CONFIG_TEST_CLOCK_MONITOR_TOLERANCE_PPM,
		.window_ns     = CONFIG_TEST_CLOCK_MONITOR_WINDOW_NS,
	},
};

static const struct clock_monitor_config measure_cfg = {
	.mode = CLOCK_MONITOR_MODE_MEASURE,
	.measure = {
		.window_ns = CONFIG_TEST_CLOCK_MONITOR_MEASURE_NS,
	},
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Bring a device back to IDLE. stop() alone only undoes RUNNING ->
 * CONFIGURED; configure(DISABLED) is what tears the HW down and resets
 * the state machine so the next case's preconditions hold.
 */
static void clean_slate(const struct device *dev)
{
	(void)clock_monitor_stop(dev);

	struct clock_monitor_config disabled = {
		.mode = CLOCK_MONITOR_MODE_DISABLED,
	};
	(void)clock_monitor_configure(dev, &disabled);

	uint32_t evts;
	(void)clock_monitor_get_events(dev, &evts);
}

/*
 * Probe whether the back-end accepts `mode` by issuing a minimal valid
 * configure(); restores the device to IDLE before returning.
 *
 * -ENOTSUP from configure() means the back-end does not implement the
 * mode; any other status (including 0) means it does.
 */
static bool dev_supports_mode(const struct device *dev,
			      enum clock_monitor_mode mode)
{
	struct clock_monitor_config cfg;
	int ret;

	switch (mode) {
	case CLOCK_MONITOR_MODE_WINDOW:
		cfg = window_cfg;
		break;
	case CLOCK_MONITOR_MODE_MEASURE:
		cfg = measure_cfg;
		break;
	default:
		return false;
	}

	ret = clock_monitor_configure(dev, &cfg);
	clean_slate(dev);
	return ret != -ENOTSUP;
}

typedef void (*case_body_t)(const struct device *dev);

/*
 * Run `body` on every DT-listed device that supports `mode`. Skip the
 * whole case if no device matches.
 */
static void foreach_device_with_mode(enum clock_monitor_mode mode,
				     case_body_t body)
{
	bool ran = false;

	for (size_t i = 0; i < NUM_CLOCK_DEVICES; i++) {
		const struct device *dev = clock_devices[i];

		zassert_true(device_is_ready(dev), "%s not ready", dev->name);

		if (!dev_supports_mode(dev, mode)) {
			continue;
		}
		clean_slate(dev);
		body(dev);
		ran = true;
	}
	if (!ran) {
		ztest_test_skip();
	}
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	for (size_t i = 0; i < NUM_CLOCK_DEVICES; i++) {
		clean_slate(clock_devices[i]);
	}
}

ZTEST_SUITE(clock_monitor_api, NULL, NULL, before_each, NULL, NULL);

/* ------------------------------------------------------------------ */
/* WINDOW path                                                         */
/* ------------------------------------------------------------------ */

static void body_window_check_stop(const struct device *dev)
{
	struct clock_monitor_config cfg = window_cfg;

	zassert_ok(clock_monitor_configure(dev, &cfg),
		   "WINDOW configure failed on %s", dev->name);
	zassert_ok(clock_monitor_start(dev), "start failed on %s", dev->name);
	zassert_ok(clock_monitor_stop(dev), "stop failed on %s", dev->name);
}
ZTEST(clock_monitor_api, test_window_check_stop)
{
	foreach_device_with_mode(CLOCK_MONITOR_MODE_WINDOW,
				 body_window_check_stop);
}

static void body_window_configure_while_running(const struct device *dev)
{
	struct clock_monitor_config cfg = window_cfg;

	zassert_ok(clock_monitor_configure(dev, &cfg), NULL);
	zassert_ok(clock_monitor_start(dev), NULL);

	zassert_equal(clock_monitor_configure(dev, &cfg), -EBUSY,
		      "configure while running must return -EBUSY on %s",
		      dev->name);
	zassert_ok(clock_monitor_stop(dev), NULL);
}
ZTEST(clock_monitor_api, test_window_configure_while_running)
{
	foreach_device_with_mode(CLOCK_MONITOR_MODE_WINDOW,
				 body_window_configure_while_running);
}

/* ------------------------------------------------------------------ */
/* MEASURE path                                                        */
/* ------------------------------------------------------------------ */

static void body_measure_configure_ok(const struct device *dev)
{
	struct clock_monitor_config cfg = measure_cfg;

	zassert_ok(clock_monitor_configure(dev, &cfg),
		   "MEASURE configure failed on %s", dev->name);
}
ZTEST(clock_monitor_api, test_measure_configure_ok)
{
	foreach_device_with_mode(CLOCK_MONITOR_MODE_MEASURE,
				 body_measure_configure_ok);
}

static void body_measure_oneshot(const struct device *dev)
{
	struct clock_monitor_config cfg = measure_cfg;

	zassert_ok(clock_monitor_configure(dev, &cfg), NULL);

	uint32_t hz = 0U;

	zassert_ok(clock_monitor_measure(dev, &hz, K_MSEC(50)),
		   "measure() failed on %s", dev->name);
	zassert_true(hz > 0U, "measured hz must be > 0 on %s (got %u)",
		     dev->name, hz);

	uint32_t evts = 0U;

	zassert_ok(clock_monitor_get_events(dev, &evts), NULL);
	zassert_true((evts & CLOCK_MONITOR_EVT_MEASURE_DONE) != 0U,
		     "MEASURE_DONE not latched on %s (got 0x%x)",
		     dev->name, evts);

	zassert_ok(clock_monitor_get_events(dev, &evts), NULL);
	zassert_equal(evts, 0U, "second get_events must be 0 on %s (got 0x%x)",
		      dev->name, evts);
}
ZTEST(clock_monitor_api, test_measure_oneshot)
{
	foreach_device_with_mode(CLOCK_MONITOR_MODE_MEASURE,
				 body_measure_oneshot);
}

static void body_measure_tight_timeout(const struct device *dev)
{
	struct clock_monitor_config cfg = measure_cfg;

	zassert_ok(clock_monitor_configure(dev, &cfg), NULL);

	uint32_t hz;
	int ret = clock_monitor_measure(dev, &hz, K_NO_WAIT);

	zassert_equal(ret, -EAGAIN,
		      "measure() with no wait must return -EAGAIN on %s "
		      "(got %d)", dev->name, ret);
}
ZTEST(clock_monitor_api, test_measure_tight_timeout)
{
	foreach_device_with_mode(CLOCK_MONITOR_MODE_MEASURE,
				 body_measure_tight_timeout);
}

static void body_measure_without_configure(const struct device *dev)
{
	uint32_t hz;
	/* before_each + foreach's clean_slate both ran, so state is IDLE. */
	zassert_equal(clock_monitor_measure(dev, &hz, K_MSEC(1)), -EINVAL,
		      "measure() on unconfigured %s must return -EINVAL",
		      dev->name);
}
ZTEST(clock_monitor_api, test_measure_without_configure)
{
	foreach_device_with_mode(CLOCK_MONITOR_MODE_MEASURE,
				 body_measure_without_configure);
}

/* ------------------------------------------------------------------ */
/* Cross-mode rejection: a device that supports only one of the modes  */
/* must reject the other with -ENOTSUP.                                */
/* ------------------------------------------------------------------ */

ZTEST(clock_monitor_api, test_rejects_unsupported_mode)
{
	if (NUM_CLOCK_DEVICES == 0U) {
		ztest_test_skip();
	}
	bool ran = false;

	for (size_t i = 0; i < NUM_CLOCK_DEVICES; i++) {
		const struct device *dev = clock_devices[i];
		bool sup_window = dev_supports_mode(dev,
						    CLOCK_MONITOR_MODE_WINDOW);
		bool sup_measure = dev_supports_mode(dev,
					     CLOCK_MONITOR_MODE_MEASURE);

		clean_slate(dev);

		if (!sup_window) {
			struct clock_monitor_config cfg = window_cfg;

			zassert_equal(clock_monitor_configure(dev, &cfg),
				      -ENOTSUP,
				      "WINDOW on non-WINDOW-capable %s "
				      "must be -ENOTSUP", dev->name);
			ran = true;
		}
		if (!sup_measure) {
			struct clock_monitor_config cfg = measure_cfg;

			zassert_equal(clock_monitor_configure(dev, &cfg),
				      -ENOTSUP,
				      "MEASURE on non-MEASURE-capable %s "
				      "must be -ENOTSUP", dev->name);
			ran = true;
		}
	}
	if (!ran) {
		/* Every listed device supports both modes; nothing to reject. */
		ztest_test_skip();
	}
}

/* ------------------------------------------------------------------ */
/* Shared API contracts                                                */
/* ------------------------------------------------------------------ */

ZTEST(clock_monitor_api, test_stop_while_idle)
{
	if (NUM_CLOCK_DEVICES == 0U) {
		ztest_test_skip();
	}
	for (size_t i = 0; i < NUM_CLOCK_DEVICES; i++) {
		const struct device *dev = clock_devices[i];

		zassert_ok(clock_monitor_stop(dev),
			   "idle stop() must be a no-op on %s", dev->name);
	}
}

ZTEST(clock_monitor_api, test_get_events_initial)
{
	if (NUM_CLOCK_DEVICES == 0U) {
		ztest_test_skip();
	}
	for (size_t i = 0; i < NUM_CLOCK_DEVICES; i++) {
		const struct device *dev = clock_devices[i];
		uint32_t evts = 0xFFFFFFFFU;

		zassert_ok(clock_monitor_get_events(dev, &evts), NULL);
		zassert_equal(evts, 0U,
			      "fresh get_events must be 0 on %s (got 0x%x)",
			      dev->name, evts);
	}
}

