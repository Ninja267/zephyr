/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>

#include <zephyr/mp/core/mp_value.h>

LOG_MODULE_REGISTER(mp_value, CONFIG_MP_LOG_LEVEL);

/**
 * A fraction is stored as a numerator/denominator pair. The int/uint union
 * lets the comparison helpers pick the correct signedness through _Generic
 * while keeping a single in-memory representation.
 */
struct mp_frac {
	union {
		int32_t v_int;
		uint32_t v_uint;
	} num;
	union {
		int32_t v_int;
		uint32_t v_uint;
	} denom;
};

/**
 * Single fixed-size value container.
 *
 * Replaces the previous "base struct + several derived structs allocated at
 * their exact size" scheme. Every value now has the same size and is taken
 * from a static pool, so there is no heap usage and no per-allocation
 * overhead. The intrusive @ref node removes the need for a separate list-node
 * wrapper: a value links itself directly into a list.
 */
struct mp_value {
	uint8_t type;     /**< enum mp_value_type */
	sys_snode_t node; /**< list linkage, valid only while the value is a list element */
	union {
		bool v_boolean;
		int32_t v_int;
		uint32_t v_uint;
		const char *v_cstring;
		struct mp_object *v_obj;
		void *v_ptr;
		struct {
			union {
				int32_t v_int;
				uint32_t v_uint;
			} min, max, step;
		} range;
		struct mp_frac frac;
		struct {
			struct mp_frac min, max, step;
		} frac_range;
		sys_slist_t v_list;
	};
};

/* Static value pool: no dynamic memory allocation. */
K_MEM_SLAB_DEFINE_STATIC(mp_value_slab, ROUND_UP(sizeof(struct mp_value), sizeof(void *)),
			 CONFIG_MP_VALUE_POOL_SIZE, sizeof(void *));

#define mp_compare(a, b)                                                                           \
	({                                                                                         \
		__typeof__(a) _a = (a);                                                            \
		__typeof__(b) _b = (b);                                                            \
		(_a < _b) ? MP_VALUE_LESS_THAN                                                     \
			  : ((_a > _b) ? MP_VALUE_GREATER_THAN : MP_VALUE_EQUAL);                  \
	})

#define mp_fraction_compare(a_num, a_den, b_num, b_den)                                            \
	({                                                                                         \
		__typeof__(a_num) _a_num = (a_num);                                                \
		__typeof__(a_den) _a_den = (a_den);                                                \
		__typeof__(b_num) _b_num = (b_num);                                                \
		__typeof__(b_den) _b_den = (b_den);                                                \
		__typeof__(a_num) sign_a_positive = ((_a_num < 0) ^ (_a_den < 0));                 \
		__typeof__(b_num) sign_b_positive = ((_b_num < 0) ^ (_b_den < 0));                 \
		_Generic((_a_num),                                                                 \
			int32_t: ((sign_a_positive != sign_b_positive)                             \
					  ? (sign_a_positive ? MP_VALUE_LESS_THAN                   \
							     : MP_VALUE_GREATER_THAN)              \
					  : mp_compare((int64_t)_a_num * (int64_t)_b_den,          \
						       (int64_t)_b_num * (int64_t)_a_den)),        \
			uint32_t: mp_compare((uint64_t)_a_num * (uint64_t)_b_den,                  \
					     (uint64_t)_b_num * (uint64_t)_a_den));                \
	})

static const uint32_t mp_value_intersect_mask[MP_TYPE_COUNT] = {
	[MP_TYPE_NONE] = 0,
	[MP_TYPE_BOOLEAN] = BIT(MP_TYPE_BOOLEAN) | BIT(MP_TYPE_LIST),
	[MP_TYPE_ENUM] = BIT(MP_TYPE_ENUM) | BIT(MP_TYPE_LIST),
	[MP_TYPE_INT] = BIT(MP_TYPE_INT) | BIT(MP_TYPE_INT_RANGE) | BIT(MP_TYPE_LIST),
	[MP_TYPE_UINT] = BIT(MP_TYPE_UINT) | BIT(MP_TYPE_UINT_RANGE) | BIT(MP_TYPE_LIST),
	[MP_TYPE_UINT_FRACTION] =
		BIT(MP_TYPE_UINT_FRACTION) | BIT(MP_TYPE_UINT_FRACTION_RANGE) | BIT(MP_TYPE_LIST),
	[MP_TYPE_INT_FRACTION] =
		BIT(MP_TYPE_INT_FRACTION) | BIT(MP_TYPE_INT_FRACTION_RANGE) | BIT(MP_TYPE_LIST),
	[MP_TYPE_STRING] = BIT(MP_TYPE_STRING) | BIT(MP_TYPE_LIST),
	[MP_TYPE_INT_RANGE] = BIT(MP_TYPE_INT) | BIT(MP_TYPE_INT_RANGE) | BIT(MP_TYPE_LIST),
	[MP_TYPE_UINT_RANGE] = BIT(MP_TYPE_UINT) | BIT(MP_TYPE_UINT_RANGE) | BIT(MP_TYPE_LIST),
	[MP_TYPE_UINT_FRACTION_RANGE] =
		BIT(MP_TYPE_UINT_FRACTION) | BIT(MP_TYPE_UINT_FRACTION_RANGE) | BIT(MP_TYPE_LIST),
	[MP_TYPE_INT_FRACTION_RANGE] =
		BIT(MP_TYPE_INT_FRACTION) | BIT(MP_TYPE_INT_FRACTION_RANGE) | BIT(MP_TYPE_LIST),
	[MP_TYPE_LIST] = BIT(MP_TYPE_BOOLEAN) | BIT(MP_TYPE_ENUM) | BIT(MP_TYPE_INT) |
			 BIT(MP_TYPE_UINT) | BIT(MP_TYPE_UINT_FRACTION) | BIT(MP_TYPE_INT_FRACTION) |
			 BIT(MP_TYPE_STRING) | BIT(MP_TYPE_INT_RANGE) | BIT(MP_TYPE_UINT_RANGE) |
			 BIT(MP_TYPE_UINT_FRACTION_RANGE) | BIT(MP_TYPE_INT_FRACTION_RANGE) |
			 BIT(MP_TYPE_LIST),
	[MP_TYPE_OBJECT] = 0,
	[MP_TYPE_PTR] = 0,
};

static struct mp_value *mp_value_alloc(void)
{
	struct mp_value *value;

	if (k_mem_slab_alloc(&mp_value_slab, (void **)&value, K_NO_WAIT) != 0) {
		LOG_ERR("mp_value pool exhausted (CONFIG_MP_VALUE_POOL_SIZE=%d)",
			CONFIG_MP_VALUE_POOL_SIZE);
		return NULL;
	}

	memset(value, 0, sizeof(*value));

	return value;
}

enum mp_value_type mp_value_get_type(const struct mp_value *value)
{
	return value->type;
}

bool mp_value_is_primitive(const struct mp_value *value)
{
	if (value == NULL || !IN_RANGE(value->type, MP_TYPE_NONE + 1, MP_TYPE_COUNT - 1)) {
		return false;
	}

	return ((BIT(MP_TYPE_BOOLEAN) | BIT(MP_TYPE_ENUM) | BIT(MP_TYPE_INT) | BIT(MP_TYPE_UINT) |
		 BIT(MP_TYPE_INT_FRACTION) | BIT(MP_TYPE_UINT_FRACTION) | BIT(MP_TYPE_STRING)) &
		BIT(value->type)) != 0;
}

static void mp_frac_reduce(struct mp_frac *frac, bool is_uint)
{
	uint32_t gcd;

	if (is_uint) {
		gcd = sys_gcd(frac->num.v_uint, frac->denom.v_uint);
		if (gcd != 0) {
			frac->num.v_uint /= gcd;
			frac->denom.v_uint /= gcd;
		}
	} else {
		gcd = sys_gcd(frac->num.v_int, frac->denom.v_int);
		if (gcd != 0) {
			frac->num.v_int /= gcd;
			frac->denom.v_int /= gcd;
		}
	}
}

static void mp_frac_set(struct mp_frac *frac, bool is_uint, va_list *args)
{
	frac->num.v_uint = va_arg(*args, uint32_t);
	frac->denom.v_uint = va_arg(*args, uint32_t);
	__ASSERT_NO_MSG(frac->denom.v_uint != 0);

	mp_frac_reduce(frac, is_uint);
}

static void mp_value_set_range(struct mp_value *value, int type, va_list *args)
{
	value->type = type;
	value->range.min.v_uint = va_arg(*args, uint32_t);
	value->range.max.v_uint = va_arg(*args, uint32_t);
	value->range.step.v_uint = va_arg(*args, uint32_t);
}

static void mp_value_set_fraction(struct mp_value *value, int type, va_list *args)
{
	value->type = type;
	mp_frac_set(&value->frac, type == MP_TYPE_UINT_FRACTION, args);
}

static void mp_value_set_fraction_range(struct mp_value *value, int type, va_list *args)
{
	bool is_uint = (type == MP_TYPE_UINT_FRACTION_RANGE);

	value->type = type;
	mp_frac_set(&value->frac_range.min, is_uint, args);
	mp_frac_set(&value->frac_range.max, is_uint, args);
	mp_frac_set(&value->frac_range.step, is_uint, args);
}

static void mp_value_set_list(struct mp_value *value, va_list *args)
{
	struct mp_value *list_item;

	while ((list_item = va_arg(*args, struct mp_value *)) != NULL) {
		mp_value_list_append(value, list_item);
	}
}

static void mp_value_set_va_list(struct mp_value *value, int type, va_list *args)
{
	if (value == NULL) {
		return;
	}

	value->type = type;
	switch (value->type) {
	case MP_TYPE_BOOLEAN:
	case MP_TYPE_ENUM:
	case MP_TYPE_INT:
		value->v_int = va_arg(*args, int);
		break;
	case MP_TYPE_STRING:
		value->v_cstring = va_arg(*args, const char *);
		break;
	case MP_TYPE_UINT:
		value->v_uint = va_arg(*args, uint32_t);
		break;
	case MP_TYPE_OBJECT:
		mp_object_replace(&value->v_obj, va_arg(*args, struct mp_object *));
		break;
	case MP_TYPE_PTR:
		value->v_ptr = va_arg(*args, void *);
		break;
	case MP_TYPE_UINT_FRACTION:
	case MP_TYPE_INT_FRACTION:
		mp_value_set_fraction(value, type, args);
		break;
	case MP_TYPE_INT_RANGE:
	case MP_TYPE_UINT_RANGE:
		mp_value_set_range(value, type, args);
		break;
	case MP_TYPE_UINT_FRACTION_RANGE:
	case MP_TYPE_INT_FRACTION_RANGE:
		mp_value_set_fraction_range(value, type, args);
		break;
	case MP_TYPE_LIST:
		mp_value_set_list(value, args);
		break;
	default:
		break;
	}
}

void mp_value_set(struct mp_value *value, int type, ...)
{
	va_list args;

	va_start(args, type);
	mp_value_set_va_list(value, type, &args);
	va_end(args);
}

int mp_value_get_fraction_numerator(const struct mp_value *frac)
{
	return frac->frac.num.v_int;
}

int mp_value_get_fraction_denominator(const struct mp_value *frac)
{
	return frac->frac.denom.v_int;
}

struct mp_fraction mp_value_get_fraction_range_min(const struct mp_value *fraction_range)
{
	return (struct mp_fraction){.num = fraction_range->frac_range.min.num.v_int,
				    .denom = fraction_range->frac_range.min.denom.v_int};
}

struct mp_fraction mp_value_get_fraction_range_max(const struct mp_value *fraction_range)
{
	return (struct mp_fraction){.num = fraction_range->frac_range.max.num.v_int,
				    .denom = fraction_range->frac_range.max.denom.v_int};
}

struct mp_fraction mp_value_get_fraction_range_step(const struct mp_value *fraction_range)
{
	return (struct mp_fraction){.num = fraction_range->frac_range.step.num.v_int,
				    .denom = fraction_range->frac_range.step.denom.v_int};
}

const char *mp_value_get_string(const struct mp_value *value)
{
	return value->v_cstring;
}

int mp_value_get_int(const struct mp_value *value)
{
	return value->v_int;
}

uint32_t mp_value_get_uint(const struct mp_value *value)
{
	return value->v_uint;
}

void *mp_value_get_ptr(const struct mp_value *value)
{
	return value ? value->v_ptr : NULL;
}

bool mp_value_get_boolean(const struct mp_value *value)
{
	return value->v_boolean;
}

struct mp_value *mp_value_new_empty(enum mp_value_type type)
{
	struct mp_value *value;

	if (type >= MP_TYPE_COUNT || type < MP_TYPE_NONE) {
		LOG_ERR("Invalid value type: %d", type);
		return NULL;
	}

	value = mp_value_alloc();
	if (value == NULL) {
		return NULL;
	}

	value->type = type;
	if (value->type == MP_TYPE_LIST) {
		sys_slist_init(&value->v_list);
	}

	return value;
}

void mp_value_destroy(struct mp_value *value)
{
	if (value == NULL) {
		return;
	}

	if (value->type == MP_TYPE_LIST) {
		sys_snode_t *node;

		while ((node = sys_slist_get(&value->v_list)) != NULL) {
			mp_value_destroy(CONTAINER_OF(node, struct mp_value, node));
		}
	} else if (value->type == MP_TYPE_OBJECT) {
		mp_object_unref(value->v_obj);
	}

	k_mem_slab_free(&mp_value_slab, value);
}

struct mp_value *mp_value_new(enum mp_value_type type, ...)
{

	struct mp_value *value;
	va_list args;

	va_start(args, type);

	value = mp_value_new_va_list(type, &args);

	va_end(args);

	return value;
}

struct mp_value *mp_value_new_va_list(enum mp_value_type type, va_list *args)
{
	struct mp_value *value = mp_value_new_empty(type);

	mp_value_set_va_list(value, type, args);

	return value;
}

static bool mp_value_copy(struct mp_value *dst, const struct mp_value *src)
{
	if (src->type == MP_TYPE_LIST) {
		struct mp_value *item;

		SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&src->v_list, item, node) {
			struct mp_value *dup = mp_value_duplicate(item);

			if (dup == NULL) {
				return false;
			}
			mp_value_list_append(dst, dup);
		}
	} else if (src->type == MP_TYPE_OBJECT) {
		dst->v_obj = src->v_obj;
		mp_object_ref(dst->v_obj);
	} else {
		sys_snode_t node = dst->node;

		/* Plain (non-owning) payload: copy the whole union in one shot
		 * while preserving the destination's own list linkage.
		 */
		*dst = *src;
		dst->node = node;
	}

	return true;
}

struct mp_value *mp_value_duplicate(const struct mp_value *value)
{
	struct mp_value *dup_value;

	if (value == NULL) {
		return NULL;
	}

	dup_value = mp_value_new_empty(value->type);
	if (dup_value == NULL) {
		return NULL;
	}

	if (!mp_value_copy(dup_value, value)) {
		mp_value_destroy(dup_value);
		return NULL;
	}

	return dup_value;
}

void mp_value_list_append(struct mp_value *list, struct mp_value *append_value)
{
	__ASSERT_NO_MSG(append_value != NULL && list != NULL);

	if (list == NULL || append_value == NULL) {
		return;
	}

	sys_slist_append(&list->v_list, &append_value->node);
}

struct mp_value *mp_value_list_get(const struct mp_value *list, int index)
{
	struct mp_value *item;
	int count = 0;

	SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&list->v_list, item, node) {
		if (count++ == index) {
			return item;
		}
	}

	return NULL;
}

bool mp_value_list_is_empty(const struct mp_value *list)
{
	return sys_slist_is_empty((sys_slist_t *)&list->v_list);
}

size_t mp_value_list_get_size(const struct mp_value *list)
{
	return sys_slist_len((sys_slist_t *)&list->v_list);
}

int mp_value_get_int_range_min(const struct mp_value *range)
{
	return range->range.min.v_int;
}

int mp_value_get_int_range_max(const struct mp_value *range)
{
	return range->range.max.v_int;
}

int mp_value_get_int_range_step(const struct mp_value *range)
{
	return range->range.step.v_int;
}

uint32_t mp_value_get_uint_range_min(const struct mp_value *range)
{
	return range->range.min.v_uint;
}

uint32_t mp_value_get_uint_range_max(const struct mp_value *range)
{
	return range->range.max.v_uint;
}

uint32_t mp_value_get_uint_range_step(const struct mp_value *range)
{
	return range->range.step.v_uint;
}

struct mp_object *mp_value_get_object(struct mp_value *value)
{
	return value ? value->v_obj : NULL;
}

static int mp_frac_compare(const struct mp_frac *frac1, const struct mp_frac *frac2, bool is_uint)
{
	if (is_uint) {
		return mp_fraction_compare(frac1->num.v_uint, frac1->denom.v_uint, frac2->num.v_uint,
					   frac2->denom.v_uint);
	}

	return mp_fraction_compare(frac1->num.v_int, frac1->denom.v_int, frac2->num.v_int,
				   frac2->denom.v_int);
}

int mp_value_compare_fraction(const struct mp_value *frac1, const struct mp_value *frac2)
{
	if (frac1->type == MP_TYPE_INT_FRACTION && frac2->type == MP_TYPE_INT_FRACTION) {
		return mp_frac_compare(&frac1->frac, &frac2->frac, false);
	}

	if (frac1->type == MP_TYPE_UINT_FRACTION && frac2->type == MP_TYPE_UINT_FRACTION) {
		return mp_frac_compare(&frac1->frac, &frac2->frac, true);
	}

	return MP_VALUE_COMPARE_FAILED;
}

static int mp_value_list_compare(const struct mp_value *list1, const struct mp_value *list2);

int mp_value_compare(const struct mp_value *val1, const struct mp_value *val2)
{
	bool is_equal;
	bool is_uint;

	if (val1->type != val2->type) {
		return MP_VALUE_COMPARE_FAILED;
	}

	switch (val1->type) {
	case MP_TYPE_BOOLEAN:
	case MP_TYPE_ENUM:
		return val1->v_uint == val2->v_uint ? MP_VALUE_EQUAL : MP_VALUE_UNORDERED;
	case MP_TYPE_INT:
		return mp_compare(val1->v_int, val2->v_int);
	case MP_TYPE_UINT:
		return mp_compare(val1->v_uint, val2->v_uint);
	case MP_TYPE_UINT_FRACTION:
	case MP_TYPE_INT_FRACTION:
		return mp_value_compare_fraction(val1, val2);
	case MP_TYPE_STRING:
		return strcmp(val1->v_cstring, val2->v_cstring) == 0 ? MP_VALUE_EQUAL
								     : MP_VALUE_UNORDERED;
	case MP_TYPE_UINT_RANGE:
	case MP_TYPE_INT_RANGE:
		is_equal = (val1->range.min.v_uint == val2->range.min.v_uint &&
			    val1->range.max.v_uint == val2->range.max.v_uint &&
			    val1->range.step.v_uint == val2->range.step.v_uint);

		return is_equal ? MP_VALUE_EQUAL : MP_VALUE_UNORDERED;
	case MP_TYPE_INT_FRACTION_RANGE:
	case MP_TYPE_UINT_FRACTION_RANGE:
		is_uint = (val1->type == MP_TYPE_UINT_FRACTION_RANGE);
		is_equal = mp_frac_compare(&val1->frac_range.min, &val2->frac_range.min, is_uint) ==
				   MP_VALUE_EQUAL &&
			   mp_frac_compare(&val1->frac_range.max, &val2->frac_range.max, is_uint) ==
				   MP_VALUE_EQUAL &&
			   mp_frac_compare(&val1->frac_range.step, &val2->frac_range.step,
					   is_uint) == MP_VALUE_EQUAL;

		return is_equal ? MP_VALUE_EQUAL : MP_VALUE_UNORDERED;
	case MP_TYPE_LIST:
		return mp_value_list_compare(val1, val2);
	default:
		return MP_VALUE_COMPARE_FAILED;
	}
}

static int mp_value_list_compare(const struct mp_value *list1, const struct mp_value *list2)
{
	int size1 = mp_value_list_get_size(list1);
	int size2 = mp_value_list_get_size(list2);
	int count_matched = 0;
	struct mp_value *item1, *item2;

	if (list1->type != MP_TYPE_LIST || list2->type != MP_TYPE_LIST) {
		return MP_VALUE_COMPARE_FAILED;
	}

	if (size1 != size2) {
		return MP_VALUE_UNORDERED;
	}

	SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&list1->v_list, item1, node) {
		SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&list2->v_list, item2, node) {
			if (mp_value_compare(item1, item2) == MP_VALUE_EQUAL) {
				count_matched++;
			}
		}
	}

	return count_matched == size1 ? MP_VALUE_EQUAL : MP_VALUE_UNORDERED;
}

bool mp_value_can_intersect(const struct mp_value *val1, const struct mp_value *val2)
{
	if (val1 == NULL || val2 == NULL ||
	    !IN_RANGE(val1->type, MP_TYPE_NONE, MP_TYPE_COUNT - 1)) {
		return false;
	}

	return (mp_value_intersect_mask[val1->type] & BIT(val2->type)) != 0;
}

static struct mp_value *mp_value_intersect_range(const struct mp_value *ref_val,
						 const struct mp_value *compare_val, bool is_uint)
{
	if (is_uint) {
		if (ref_val->range.min.v_uint > compare_val->range.max.v_uint ||
		    compare_val->range.min.v_uint > ref_val->range.max.v_uint) {
			return NULL;
		}

		return mp_value_new(
			MP_TYPE_UINT_RANGE,
			MAX(ref_val->range.min.v_uint, compare_val->range.min.v_uint),
			MIN(ref_val->range.max.v_uint, compare_val->range.max.v_uint),
			sys_gcd(ref_val->range.step.v_uint, compare_val->range.step.v_uint));
	}

	if (ref_val->range.min.v_int > compare_val->range.max.v_int ||
	    compare_val->range.min.v_int > ref_val->range.max.v_int) {
		return NULL;
	}

	return mp_value_new(MP_TYPE_INT_RANGE,
			    MAX(ref_val->range.min.v_int, compare_val->range.min.v_int),
			    MIN(ref_val->range.max.v_int, compare_val->range.max.v_int),
			    sys_gcd(ref_val->range.step.v_int, compare_val->range.step.v_int));
}

struct mp_value *mp_value_intersect_int_range(const struct mp_value *ref_val,
					      const struct mp_value *compare_val)
{
	if (ref_val->type == MP_TYPE_INT_RANGE && compare_val->type == MP_TYPE_INT_RANGE) {
		return mp_value_intersect_range(ref_val, compare_val, false);
	}

	if (ref_val->type == MP_TYPE_UINT_RANGE && compare_val->type == MP_TYPE_UINT_RANGE) {
		return mp_value_intersect_range(ref_val, compare_val, true);
	}

	if (ref_val->type == MP_TYPE_INT_RANGE && compare_val->type == MP_TYPE_INT &&
	    IN_RANGE(compare_val->v_int, ref_val->range.min.v_int, ref_val->range.max.v_int)) {
		return mp_value_new(MP_TYPE_INT, compare_val->v_int);
	}

	if (ref_val->type == MP_TYPE_UINT_RANGE && compare_val->type == MP_TYPE_UINT &&
	    IN_RANGE(compare_val->v_uint, ref_val->range.min.v_uint, ref_val->range.max.v_uint)) {
		return mp_value_new(MP_TYPE_UINT, compare_val->v_uint);
	}

	return NULL;
}

/**
 * Pick the min or max of two fractions.
 *
 * @param frac1 the first fraction
 * @param frac2 the second fraction
 * @param is_uint compare as unsigned fractions
 * @param find_min true to return the smaller fraction, false for the larger
 * @return pointer to the selected fraction
 */
static const struct mp_frac *mp_frac_min_max(const struct mp_frac *frac1,
					     const struct mp_frac *frac2, bool is_uint,
					     bool find_min)
{
	switch (mp_frac_compare(frac1, frac2, is_uint)) {
	case MP_VALUE_LESS_THAN:
		return find_min ? frac1 : frac2;
	case MP_VALUE_GREATER_THAN:
		return find_min ? frac2 : frac1;
	default:
		return frac1;
	}
}

static bool mp_frac_range_overlap(const struct mp_value *ref_val,
				  const struct mp_value *compare_val, bool is_uint)
{
	return !(mp_frac_compare(&ref_val->frac_range.min, &compare_val->frac_range.max, is_uint) ==
			 MP_VALUE_GREATER_THAN ||
		 mp_frac_compare(&ref_val->frac_range.max, &compare_val->frac_range.min, is_uint) ==
			 MP_VALUE_LESS_THAN);
}

static bool mp_frac_in_range(const struct mp_frac *frac, const struct mp_value *range, bool is_uint)
{
	return !(mp_frac_compare(frac, &range->frac_range.min, is_uint) == MP_VALUE_LESS_THAN ||
		 mp_frac_compare(frac, &range->frac_range.max, is_uint) == MP_VALUE_GREATER_THAN);
}

struct mp_value *mp_value_intersect_fraction_range(const struct mp_value *ref_val,
						   const struct mp_value *compare_val)
{
	struct mp_value *intersect_value;
	bool is_uint = (ref_val->type == MP_TYPE_UINT_FRACTION_RANGE);

	if ((compare_val->type == MP_TYPE_UINT_FRACTION_RANGE ||
	     compare_val->type == MP_TYPE_INT_FRACTION_RANGE) &&
	    mp_frac_range_overlap(ref_val, compare_val, is_uint)) {
		intersect_value = mp_value_new_empty(ref_val->type);
		if (intersect_value == NULL) {
			return NULL;
		}

		intersect_value->frac_range.min = *mp_frac_min_max(
			&ref_val->frac_range.min, &compare_val->frac_range.min, is_uint, false);
		intersect_value->frac_range.max = *mp_frac_min_max(
			&ref_val->frac_range.max, &compare_val->frac_range.max, is_uint, true);

		if (is_uint) {
			intersect_value->frac_range.step.num.v_uint =
				sys_gcd(ref_val->frac_range.step.num.v_uint,
					compare_val->frac_range.step.num.v_uint);
			intersect_value->frac_range.step.denom.v_uint =
				sys_lcm(ref_val->frac_range.step.denom.v_uint,
					compare_val->frac_range.step.denom.v_uint);
		} else {
			intersect_value->frac_range.step.num.v_int =
				sys_gcd(ref_val->frac_range.step.num.v_int,
					compare_val->frac_range.step.num.v_int);
			intersect_value->frac_range.step.denom.v_int =
				sys_lcm(ref_val->frac_range.step.denom.v_int,
					compare_val->frac_range.step.denom.v_int);
		}
		mp_frac_reduce(&intersect_value->frac_range.step, is_uint);

		return intersect_value;
	}

	if ((compare_val->type == MP_TYPE_INT_FRACTION ||
	     compare_val->type == MP_TYPE_UINT_FRACTION) &&
	    mp_frac_in_range(&compare_val->frac, ref_val, is_uint)) {
		return mp_value_new(is_uint ? MP_TYPE_UINT_FRACTION : MP_TYPE_INT_FRACTION,
				    compare_val->frac.num.v_uint, compare_val->frac.denom.v_uint);
	}

	return NULL;
}

struct mp_value *mp_value_intersect_list(const struct mp_value *list,
					 const struct mp_value *compare_val)
{
	struct mp_value *intersect_value = NULL;
	struct mp_value *intersect_list = NULL;
	struct mp_value *item, *cmp_item;

	if (list == NULL || compare_val == NULL || compare_val->type == MP_TYPE_NONE) {
		return NULL;
	}

	SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&list->v_list, item, node) {
		intersect_value = NULL;
		switch (compare_val->type) {
		case MP_TYPE_BOOLEAN:
		case MP_TYPE_ENUM:
		case MP_TYPE_INT:
		case MP_TYPE_UINT:
		case MP_TYPE_UINT_FRACTION:
		case MP_TYPE_INT_FRACTION:
		case MP_TYPE_STRING:
			if (mp_value_compare(compare_val, item) == MP_VALUE_EQUAL) {
				intersect_value = mp_value_duplicate(compare_val);
			}
			break;
		case MP_TYPE_INT_RANGE:
		case MP_TYPE_UINT_RANGE:
			intersect_value = mp_value_intersect_int_range(compare_val, item);
			break;
		case MP_TYPE_UINT_FRACTION_RANGE:
		case MP_TYPE_INT_FRACTION_RANGE:
			intersect_value = mp_value_intersect_fraction_range(compare_val, item);
			break;
		case MP_TYPE_LIST:
			SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&compare_val->v_list, cmp_item,
						     node) {
				if (mp_value_compare(item, cmp_item) == MP_VALUE_EQUAL) {
					intersect_value = mp_value_duplicate(cmp_item);
					break;
				}
			}
			break;
		default:
			break;
		}

		if (intersect_value != NULL) {
			if (intersect_list == NULL) {
				intersect_list = mp_value_new_empty(MP_TYPE_LIST);
				if (intersect_list == NULL) {
					mp_value_destroy(intersect_value);
					return NULL;
				}
			}
			mp_value_list_append(intersect_list, intersect_value);
		}
	}

	return intersect_list;
}

struct mp_value *mp_value_intersect(const struct mp_value *val1, const struct mp_value *val2)
{
	const struct mp_value *ref_val, *compare_val;
	struct mp_value *intersect_val = NULL;

	/* Check if intersect */
	if (!mp_value_can_intersect(val1, val2)) {
		return NULL;
	}

	/* When two values don't have the same type */
	if (val1->type >= val2->type) {
		ref_val = val1;
		compare_val = val2;
	} else {
		ref_val = val2;
		compare_val = val1;
	}

	if (mp_value_is_primitive(ref_val)) {
		if (mp_value_compare(val1, val2) == MP_VALUE_EQUAL) {
			intersect_val = mp_value_duplicate(val1);
		}
	} else {
		switch (ref_val->type) {
		case MP_TYPE_INT_RANGE:
		case MP_TYPE_UINT_RANGE:
			intersect_val = mp_value_intersect_int_range(ref_val, compare_val);
			break;
		case MP_TYPE_INT_FRACTION_RANGE:
		case MP_TYPE_UINT_FRACTION_RANGE:
			intersect_val = mp_value_intersect_fraction_range(ref_val, compare_val);
			break;
		case MP_TYPE_LIST:
			intersect_val = mp_value_intersect_list(ref_val, compare_val);
			break;
		default:
			break;
		}
	}

	return intersect_val;
}

static inline void mp_value_print_int(const struct mp_value *value)
{
	printk("%d", value->v_int);
}

static inline void mp_value_print_uint(const struct mp_value *value)
{
	printk("%u", value->v_uint);
}

static inline void mp_value_print_string(const struct mp_value *value)
{
	printk("%s", value->v_cstring);
}

static inline void mp_value_print_int_range(const struct mp_value *value)
{
	printk("[%d, %d, %d]", value->range.min.v_int, value->range.max.v_int,
	       value->range.step.v_int);
}

static inline void mp_value_print_uint_range(const struct mp_value *value)
{
	printk("[%u, %u, %u]", value->range.min.v_uint, value->range.max.v_uint,
	       value->range.step.v_uint);
}

static inline void mp_frac_print(const struct mp_frac *frac, bool is_uint)
{
	if (is_uint) {
		printk("%u/%u", frac->num.v_uint, frac->denom.v_uint);
	} else {
		printk("%d/%d", frac->num.v_int, frac->denom.v_int);
	}
}

static inline void mp_value_print_fraction(const struct mp_value *value)
{
	mp_frac_print(&value->frac, value->type == MP_TYPE_UINT_FRACTION);
}

static inline void mp_value_print_fraction_range(const struct mp_value *value)
{
	bool is_uint = (value->type == MP_TYPE_UINT_FRACTION_RANGE);

	printk("[");
	mp_frac_print(&value->frac_range.min, is_uint);
	printk(",");
	mp_frac_print(&value->frac_range.max, is_uint);
	printk(",");
	mp_frac_print(&value->frac_range.step, is_uint);
	printk("]");
}

static inline void mp_value_print_list(const struct mp_value *value)
{
	struct mp_value *item;

	printk("{");
	SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&value->v_list, item, node) {
		mp_value_print(item, false);
		if (sys_slist_peek_next(&item->node) != NULL) {
			printk(", ");
		}
	}
	printk("}");
}

void mp_value_print(const struct mp_value *value, bool new_line)
{
	typedef void (*mp_value_print_fn)(const struct mp_value *value);
	static const mp_value_print_fn mp_value_print_table[MP_TYPE_COUNT] = {
		[MP_TYPE_NONE] = NULL,
		[MP_TYPE_BOOLEAN] = mp_value_print_int,
		[MP_TYPE_ENUM] = mp_value_print_int,
		[MP_TYPE_INT] = mp_value_print_int,
		[MP_TYPE_UINT] = mp_value_print_uint,
		[MP_TYPE_UINT_FRACTION] = mp_value_print_fraction,
		[MP_TYPE_INT_FRACTION] = mp_value_print_fraction,
		[MP_TYPE_INT_RANGE] = mp_value_print_int_range,
		[MP_TYPE_UINT_RANGE] = mp_value_print_uint_range,
		[MP_TYPE_STRING] = mp_value_print_string,
		[MP_TYPE_LIST] = mp_value_print_list,
		[MP_TYPE_UINT_FRACTION_RANGE] = mp_value_print_fraction_range,
		[MP_TYPE_INT_FRACTION_RANGE] = mp_value_print_fraction_range,
		[MP_TYPE_OBJECT] = NULL,
		[MP_TYPE_PTR] = NULL,
	};

	if (value == NULL || value->type >= ARRAY_SIZE(mp_value_print_table) ||
	    mp_value_print_table[value->type] == NULL) {
		LOG_ERR("Invalid mp_value to print");
		return;
	}

	mp_value_print_fn print_fn = mp_value_print_table[value->type];

	if (print_fn) {
		print_fn(value);
	}

	if (new_line) {
		printk("\n");
	}
}
