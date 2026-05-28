/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mp/core/mp_caps.h>
#include <zephyr/mp/core/mp_structure.h>
#include <zephyr/mp/core/mp_object.h>

LOG_MODULE_REGISTER(mp_caps, CONFIG_MP_LOG_LEVEL);

/* Static caps pool: reference-counted caps objects without heap usage. */
K_MEM_SLAB_DEFINE_STATIC(mp_caps_slab, ROUND_UP(sizeof(struct mp_caps), sizeof(void *)),
			 CONFIG_MP_CAPS_POOL_SIZE, sizeof(void *));

static void mp_caps_destroy(struct mp_object *obj)
{
	struct mp_structure *structure;
	sys_snode_t *node;

	__ASSERT_NO_MSG(obj != NULL);
	while ((node = sys_slist_get(&MP_CAPS(obj)->caps_structures)) != NULL) {
		structure = CONTAINER_OF(node, struct mp_structure, node);
		mp_structure_destroy(structure);
	}
	k_mem_slab_free(&mp_caps_slab, obj);
}

void mp_caps_init(struct mp_caps *caps, uint8_t flag)
{
	__ASSERT_NO_MSG(caps != NULL);
	sys_slist_init(&caps->caps_structures);
	caps->object.release = mp_caps_destroy;
	caps->object.ref = ATOMIC_INIT(0);
	caps->object.flags = flag;
}

static struct mp_caps *mp_caps_new_empty_with_flag(uint8_t flags)
{
	struct mp_caps *caps;

	if (k_mem_slab_alloc(&mp_caps_slab, (void **)&caps, K_NO_WAIT) != 0) {
		LOG_ERR("mp_caps pool exhausted (CONFIG_MP_CAPS_POOL_SIZE=%d)",
			CONFIG_MP_CAPS_POOL_SIZE);
		return NULL;
	}

	mp_caps_init(caps, flags);
	return mp_caps_ref(caps);
}

static inline struct mp_caps *mp_caps_new_empty(void)
{
	return mp_caps_new_empty_with_flag(0);
}

struct mp_caps *mp_caps_new_any(void)
{
	return mp_caps_new_empty_with_flag(MP_CAPS_FLAG_ANY);
}

struct mp_caps *mp_caps_new(uint8_t media_type_id, ...)
{
	struct mp_caps *caps = mp_caps_new_empty();
	va_list var_args;
	struct mp_structure *structure;
	uint8_t field_id;
	enum mp_value_type type;
	struct mp_value *value;

	if (caps == NULL) {
		return NULL;
	}

	if (media_type_id == MP_MEDIA_END) {
		return caps;
	}

	structure = mp_structure_new(media_type_id, MP_CAPS_END);
	if (structure == NULL) {
		return caps;
	}

	va_start(var_args, media_type_id);
	while (1) {
		field_id = (uint8_t)va_arg(var_args, uint32_t);
		if (field_id == MP_CAPS_END) {
			break;
		}

		type = va_arg(var_args, int);
		if (type != MP_TYPE_LIST) {
			value = mp_value_new_va_list(type, &var_args);
		} else {
			value = va_arg(var_args, struct mp_value *);
		}

		if (value == NULL) {
			break;
		}

		mp_structure_append(structure, field_id, value);
	}
	va_end(var_args);

	mp_caps_append(caps, structure);

	return caps;
}

void mp_caps_replace(struct mp_caps **target_caps, struct mp_caps *new_caps)
{
	__ASSERT_NO_MSG(target_caps != NULL);

	struct mp_caps *old_caps = *target_caps;

	/* Update the target with a new reference */
	*target_caps = mp_caps_ref(new_caps);

	/* Release the old reference */
	mp_caps_unref(old_caps);
}

bool mp_caps_append(struct mp_caps *caps, struct mp_structure *structure)
{
	if (caps == NULL || caps->object.flags == MP_CAPS_FLAG_ANY || structure == NULL) {
		return false;
	}

	sys_slist_append(&caps->caps_structures, &structure->node);

	return true;
}

void mp_caps_print(struct mp_caps *caps)
{
	struct mp_structure *structure;

	if (caps == NULL) {
		printk("Caps NULL\n");
		return;
	}

	if (mp_caps_is_any(caps)) {
		printk("Caps ANY\n");
		return;
	}

	if (mp_caps_is_empty(caps)) {
		printk("Caps EMPTY\n");
		return;
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&caps->caps_structures, structure, node) {
		mp_structure_print(structure);
	}
}

bool mp_caps_is_empty(struct mp_caps *caps)
{
	return caps != NULL && caps->object.flags != MP_CAPS_FLAG_ANY &&
	       sys_slist_is_empty(&caps->caps_structures);
}

bool mp_caps_is_any(struct mp_caps *caps)
{
	return caps != NULL && caps->object.flags == MP_CAPS_FLAG_ANY;
}

bool mp_caps_is_fixed(struct mp_caps *caps)
{
	struct mp_structure *first_structure;
	int structure_count;

	if (caps == NULL) {
		return false;
	}

	structure_count = sys_slist_len(&caps->caps_structures);
	if (structure_count == 0 || structure_count > 1) {
		return false;
	}

	first_structure = mp_caps_get_structure(caps, 0);

	return first_structure ? mp_structure_is_fixed(first_structure) : false;
}

struct mp_caps *mp_caps_intersect(struct mp_caps *caps1, struct mp_caps *caps2)
{
	struct mp_caps *intersect_caps;
	struct mp_structure *cs1, *cs2;

	if (caps1 == NULL || caps2 == NULL || mp_caps_is_empty(caps1) || mp_caps_is_empty(caps2)) {
		return NULL;
	}

	if (mp_caps_is_any(caps1)) {
		return mp_caps_duplicate(caps2);
	}

	if (mp_caps_is_any(caps2)) {
		return mp_caps_duplicate(caps1);
	}

	intersect_caps = mp_caps_new_empty();
	if (intersect_caps == NULL) {
		return NULL;
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&caps1->caps_structures, cs1, node) {
		SYS_SLIST_FOR_EACH_CONTAINER(&caps2->caps_structures, cs2, node) {
			struct mp_structure *struct_intersect = mp_structure_intersect(cs1, cs2);

			if (struct_intersect) {
				mp_caps_append(intersect_caps, struct_intersect);
			}
		}
	}

	return intersect_caps;
}

bool mp_caps_can_intersect(struct mp_caps *caps1, struct mp_caps *caps2)
{

	struct mp_structure *cs1, *cs2;

	if (caps1 == NULL || caps2 == NULL || mp_caps_is_empty(caps1) || mp_caps_is_empty(caps2)) {
		return false;
	}

	if (mp_caps_is_any(caps1) || mp_caps_is_any(caps2)) {
		return true;
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&caps1->caps_structures, cs1, node) {
		SYS_SLIST_FOR_EACH_CONTAINER(&caps2->caps_structures, cs2, node) {
			if (mp_structure_can_intersect(cs1, cs2)) {
				return true;
			}
		}
	}

	return false;
}

struct mp_caps *mp_caps_duplicate(struct mp_caps *caps)
{
	struct mp_structure *cs;
	struct mp_structure *structure;
	struct mp_caps *caps_copy;

	if (caps == NULL) {
		return NULL;
	}

	if (mp_caps_is_any(caps)) {
		return mp_caps_new_any();
	}

	caps_copy = mp_caps_new_empty();
	if (caps_copy == NULL) {
		return NULL;
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&caps->caps_structures, cs, node) {
		structure = mp_structure_duplicate(cs);
		if (structure) {
			mp_caps_append(caps_copy, structure);
		}
	}

	return caps_copy;
}

struct mp_structure *mp_caps_get_structure(struct mp_caps *caps, int index)
{
	int i = 0;
	struct mp_structure *cs;

	SYS_SLIST_FOR_EACH_CONTAINER(&caps->caps_structures, cs, node) {
		if (i++ == index) {
			return cs;
		}
	}

	return NULL;
}

struct mp_caps *mp_caps_fixate(struct mp_caps *caps)
{
	sys_snode_t *node;
	struct mp_caps *fixed_caps;
	struct mp_structure *fixated_structure;
	struct mp_structure *cs;

	if (caps == NULL || mp_caps_is_any(caps) || mp_caps_is_empty(caps)) {
		return NULL;
	}

	node = sys_slist_peek_head(&caps->caps_structures);
	if (node == NULL) {
		return NULL;
	}

	fixed_caps = mp_caps_new_empty();
	if (fixed_caps == NULL) {
		return NULL;
	}

	cs = CONTAINER_OF(node, struct mp_structure, node);
	fixated_structure = mp_structure_fixate(cs);
	if (fixated_structure == NULL) {
		mp_caps_unref(fixed_caps);
		return NULL;
	}

	mp_caps_append(fixed_caps, fixated_structure);

	return fixed_caps;
}
