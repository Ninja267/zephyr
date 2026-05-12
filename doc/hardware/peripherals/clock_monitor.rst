.. _clock_monitor_api:

Clock Monitor
#############

Overview
********

The clock monitor API provides access to hardware peripherals that observe a
clock signal at runtime and report when its frequency drifts outside expected
bounds or stops entirely. It is intended for functional-safety and
diagnostic use cases — detecting failed oscillators, lost reference clocks,
or out-of-spec frequency drift on critical clock trees.

Operating Modes
***************

The API exposes two operating modes:

``CLOCK_MONITOR_MODE_WINDOW``
   Continuous threshold check. The hardware compares the monitored clock's
   frequency against programmable high and low bounds derived from
   :c:member:`clock_monitor_window_cfg.expected_hz` and
   :c:member:`clock_monitor_window_cfg.tolerance_ppm`. Threshold crossings
   are delivered asynchronously through the user callback installed at
   configure time. Started with :c:func:`clock_monitor_start` and stopped
   with :c:func:`clock_monitor_stop`.

``CLOCK_MONITOR_MODE_MEASURE``
   Single-shot frequency measurement. :c:func:`clock_monitor_measure`
   triggers one measurement, blocks the calling thread until the hardware
   completes (event-driven, not polled), and returns the measured value in
   Hz.

Events
******

Events are delivered as a bitmask in :c:member:`clock_monitor_event_data.events`:

* ``CLOCK_MONITOR_EVT_FREQ_HIGH`` — monitored frequency exceeded the
  upper threshold (WINDOW mode).
* ``CLOCK_MONITOR_EVT_FREQ_LOW`` — monitored frequency fell below the
  lower threshold (WINDOW mode).
* ``CLOCK_MONITOR_EVT_CLOCK_LOST`` — monitored clock stopped producing
  edges within the measurement window (MEASURE mode hardware failure).
* ``CLOCK_MONITOR_EVT_MEASURE_DONE`` — single-shot measurement completed
  successfully (MEASURE mode); :c:member:`clock_monitor_event_data.measured_hz`
  holds the result.

Events that occur while no callback is installed (or that arrive while the
CPU is busy) are latched in hardware and can be drained via
:c:func:`clock_monitor_get_events`, which atomically reads and clears the
latched bitmask.

Configuration
*************

A clock monitor must be configured via :c:func:`clock_monitor_configure`
before it is started. The configuration carries the operating mode, the
mode-specific parameters (expected frequency, tolerance, measurement
window) and an optional asynchronous callback. Switching modes implicitly
stops any ongoing measurement.

Configure-time return codes:

* ``0`` — success
* ``-EINVAL`` — malformed configuration
* ``-ENOTSUP`` — back-end does not support the requested mode
* ``-EBUSY`` — monitor is currently running
* ``-ENOSYS`` — back-end does not implement ``configure()``

Related configuration options:

* :kconfig:option:`CONFIG_CLOCK_MONITOR`

API Reference
*************

.. doxygengroup:: clock_monitor_interface
