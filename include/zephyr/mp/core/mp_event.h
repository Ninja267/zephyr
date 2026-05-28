/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mp
 * @brief Event header file.
 */

#ifndef ZEPHYR_INCLUDE_MP_CORE_MP_EVENT_H_
#define ZEPHYR_INCLUDE_MP_CORE_MP_EVENT_H_

#include <zephyr/mp/core/mp_caps.h>
#include <zephyr/mp/core/mp_object.h>
#include <zephyr/mp/core/mp_structure.h>

/**
 * @defgroup mp_event Pipeline Event
 * @brief Pipeline event
 *
 * @{
 */

/**
 * Create an event type from an id number and flags.
 *
 * @param num	Event id number (A new type of event must have unique number id)
 * @param flags Event flags
 */
#define MP_EVENT_CREATE_TYPE(num, flags) (((num) << 2) | (flags))

/**
 * Get the direction of an event.
 *
 * @param event Pointer to struct mp_event
 *
 * @return Direction of event, see @ref mp_event_direction
 */
#define MP_EVENT_DIRECTION(event) (event->type & 0b11)

/**
 * Event direction flags.
 */
enum mp_event_direction {
	MP_EVENT_DIRECTION_UNKNOWN = 0,           /**< Unknown direction */
	MP_EVENT_DIRECTION_UPSTREAM = BIT(0),     /**< Event flows upstream */
	MP_EVENT_DIRECTION_DOWNSTREAM = BIT(1),   /**< Event flows downstream */
	MP_EVENT_DIRECTION_ANY = BIT(1) | BIT(0), /**< Event can flow in any direction */
};

/**
 * Event types.
 */
enum mp_event_type {
	MP_EVENT_UNKNOWN = MP_EVENT_CREATE_TYPE(0, 0), /**< Unknown event */
	MP_EVENT_CAPS =
		MP_EVENT_CREATE_TYPE(1, MP_EVENT_DIRECTION_DOWNSTREAM), /**< Capabilities event */
	MP_EVENT_EOS =
		MP_EVENT_CREATE_TYPE(2, MP_EVENT_DIRECTION_DOWNSTREAM), /**< End-of-stream event */

	MP_EVENT_END = UINT8_MAX, /**< Maximum event type identifer */
};

/**
 * Event structure.
 */
struct mp_event {
	uint8_t type;                  /**< Type of the event */
	struct mp_structure structure; /**< Associated metadata structure */
	uint32_t timestamp;            /**< Timestamp of the event */
};

/**
 * @brief Initialize an event of the given type.
 *
 * The event is initialized in place; no dynamic allocation is performed for
 * the event itself.
 *
 * @param event Pointer to caller-provided storage for the @ref mp_event
 * @param type Event type to assign (See @ref mp_event_type)
 */
void mp_event_init_custom(struct mp_event *event, enum mp_event_type type);

/**
 * @brief Initialize a CAPS event.
 *
 * The event is initialized in place; no dynamic allocation is performed for
 * the event itself. The caller's reference to @p caps is consumed (the event
 * takes its own internal reference and the caller's reference is released).
 *
 * @param event Pointer to caller-provided storage for the @ref mp_event
 * @param caps @ref mp_caps to include (reference consumed)
 */
void mp_event_init_caps(struct mp_event *event, struct mp_caps *caps);

/**
 * @brief Initialize an EOS (End-of-Stream) event.
 *
 * @param event Pointer to caller-provided storage for the @ref mp_event
 */
void mp_event_init_eos(struct mp_event *event);

/**
 * @brief Release resources held by an event.
 *
 * Releases the event's internal references and clears its associated
 * structure. The @p event storage itself is not freed (it may be reused via
 * another mp_event_init_*() call).
 *
 * @param event Pointer to the @ref mp_event to clear
 */
void mp_event_clear(struct mp_event *event);

/**
 * Get @ref mp_caps from a MP_EVENT_CAPS event.
 *
 * Returns a new reference; the caller is responsible for releasing it via
 * mp_caps_unref().
 *
 * @param event Pointer to a struct mp_event
 * @return New reference to event @ref mp_caps, or NULL on error
 */
struct mp_caps *mp_event_get_caps(struct mp_event *event);

/**
 * Set caps to a @ref MP_EVENT_CAPS event.
 *
 * The caller's reference to @p caps is consumed by this function.
 *
 * @param event Pointer to a @ref mp_event
 * @param caps Pointer to a @ref mp_caps (reference consumed)
 * @return true if successful, false otherwise
 */
bool mp_event_set_caps(struct mp_event *event, struct mp_caps *caps);

/** @} */

#endif /* ZEPHYR_INCLUDE_MP_CORE_MP_EVENT_H_ */
