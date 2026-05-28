/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/kernel.h>

#include <zephyr/mp/core/mp_event.h>

void mp_event_init_custom(struct mp_event *event, enum mp_event_type type)
{
	if (event == NULL) {
		return;
	}

	event->type = type;
	mp_structure_init(&event->structure, MP_MEDIA_UNKNOWN);
	event->timestamp = k_uptime_get_32();
}

void mp_event_init_eos(struct mp_event *event)
{
	mp_event_init_custom(event, MP_EVENT_EOS);
}

void mp_event_init_caps(struct mp_event *event, struct mp_caps *caps)
{
	if (event == NULL) {
		return;
	}

	mp_event_init_custom(event, MP_EVENT_CAPS);
	mp_structure_append(&event->structure, MP_EVENT_CAPS,
			    mp_value_new(MP_TYPE_OBJECT, caps));
}

void mp_event_clear(struct mp_event *event)
{
	if (event == NULL) {
		return;
	}

	mp_structure_clear(&event->structure);
}

struct mp_caps *mp_event_get_caps(struct mp_event *event)
{
	if (event == NULL || event->type != MP_EVENT_CAPS) {
		return NULL;
	}

	return MP_CAPS(
		mp_value_get_object(mp_structure_get_value(&event->structure, MP_EVENT_CAPS)));
}

bool mp_event_set_caps(struct mp_event *event, struct mp_caps *caps)
{
	if (event == NULL || event->type != MP_EVENT_CAPS) {
		return false;
	}

	struct mp_value *value = mp_structure_get_value(&event->structure, MP_EVENT_CAPS);

	if (value) {
		mp_value_set(value, MP_TYPE_OBJECT, caps);
	} else {
		mp_structure_append(&event->structure, MP_EVENT_CAPS,
				    mp_value_new(MP_TYPE_OBJECT, caps));
	}

	return true;
}
