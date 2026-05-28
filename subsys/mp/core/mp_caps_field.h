/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_MP_CORE_MP_CAPS_FIELD_H_
#define ZEPHYR_SUBSYS_MP_CORE_MP_CAPS_FIELD_H_

#include <zephyr/mp/core/mp_caps.h>
#include <zephyr/mp/core/mp_structure.h>
#include <zephyr/mp/core/mp_value.h>

/*
 * Helpers shared by mp_query and mp_event to read/write a struct mp_caps stored
 * inside an mp_structure field, with consistent reference semantics.
 */

/**
 * Read a refed mp_caps from a structure field.
 *
 * Returns a new reference; the caller is responsible for releasing it via
 * mp_caps_unref().
 */
static inline struct mp_caps *mp_caps_field_get(struct mp_structure *structure, uint8_t field)
{
	struct mp_value *value = mp_structure_get_value(structure, field);
	struct mp_object *obj;

	if (value == NULL) {
		return NULL;
	}

	obj = mp_value_get_object(value);
	return obj ? mp_caps_ref(MP_CAPS(obj)) : NULL;
}

/**
 * Store a mp_caps into a structure field, consuming the caller's reference.
 *
 * The structure takes its own reference internally; the caller's reference is
 * released. After this call the caller must not use @p caps unless it took an
 * additional reference beforehand.
 */
static inline void mp_caps_field_set(struct mp_structure *structure, uint8_t field,
				     struct mp_caps *caps)
{
	struct mp_value *value = mp_structure_get_value(structure, field);

	if (value != NULL) {
		mp_value_set(value, MP_TYPE_OBJECT, caps);
	} else {
		mp_structure_append(structure, field, mp_value_new(MP_TYPE_OBJECT, caps));
	}

	mp_caps_unref(caps);
}

#endif /* ZEPHYR_SUBSYS_MP_CORE_MP_CAPS_FIELD_H_ */
