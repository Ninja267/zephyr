/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mp/core/mp_value.h>

LOG_MODULE_REGISTER(mp_value, CONFIG_MP_LOG_LEVEL);

/*
 * Static pool of mp_value objects. mp_value is now a fixed-size tagged
 * union, so a single slab pool can serve every value type. The pool
 * capacity is configurable via Kconfig and bounds memory usage at build
 * time — no dynamic heap allocation is used.
 */
K_MEM_SLAB_DEFINE_STATIC(mp_value_slab, sizeof(struct mp_value),
			 CONFIG_MP_VALUE_POOL_SIZE, __alignof__(struct mp_value));

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
					  ? (sign_a_positive ? MP_VALUE_LESS_THAN                  \
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
			 BIT(MP_TYPE_UINT) | BIT(MP_TYPE_UINT_FRACTION) |
			 BIT(MP_TYPE_INT_FRACTION) | BIT(MP_TYPE_STRING) | BIT(MP_TYPE_INT_RANGE) |
			 BIT(MP_TYPE_UINT_RANGE) | BIT(MP_TYPE_UINT_FRACTION_RANGE) |
			 BIT(MP_TYPE_INT_FRACTION_RANGE) | BIT(MP_TYPE_LIST),
	[MP_TYPE_OBJECT] = 0,
	[MP_TYPE_PTR] = 0,
};

bool mp_value_is_primitive(const struct mp_value *value)
{
	if (value == NULL || !IN_RANGE(value->type, MP_TYPE_NONE + 1, MP_TYPE_COUNT - 1)) {
		return false;
	}

	return ((BIT(MP_TYPE_BOOLEAN) | BIT(MP_TYPE_ENUM) | BIT(MP_TYPE_INT) | BIT(MP_TYPE_UINT) |
		 BIT(MP_TYPE_INT_FRACTION) | BIT(MP_TYPE_UINT_FRACTION) | BIT(MP_TYPE_STRING)) &
		BIT(value->type)) != 0;
}

static void mp_value_set_fraction_data(struct mp_value_fraction_data *frac, int type,
				       va_list *args)
{
	uint32_t gcd;

	frac->num.v_uint = va_arg(*args, uint32_t);
	frac->denom.v_uint = va_arg(*args, uint32_t);
	__ASSERT_NO_MSG(frac->denom.v_uint != 0);
	if (type == MP_TYPE_INT_FRACTION) {
		gcd = sys_gcd(frac->num.v_int, frac->denom.v_int);
		frac->num.v_int /= gcd;
		frac->denom.v_int /= gcd;
	} else if (type == MP_TYPE_UINT_FRACTION) {
		gcd = sys_gcd(frac->num.v_uint, frac->denom.v_uint);
		frac->num.v_uint /= gcd;
		frac->denom.v_uint /= gcd;
	} else {
		LOG_ERR("Invalid fraction type");
	}
}

static void mp_value_set_range(struct mp_value *value, int type, va_list *args)
{
	value->type = type;
	value->v_range.min.v_uint = va_arg(*args, uint32_t);
	value->v_range.max.v_uint = va_arg(*args, uint32_t);
	value->v_range.step.v_uint = va_arg(*args, uint32_t);
}

static void mp_value_set_fraction(struct mp_value *value, int type, va_list *args)
{
	value->type = type;
	mp_value_set_fraction_data(&value->v_fraction, type, args);
}

static void mp_value_set_fraction_range(struct mp_value *value, int type, va_list *args)
{
	int base_type = (type == MP_TYPE_UINT_FRACTION_RANGE) ? MP_TYPE_UINT_FRACTION
							      : MP_TYPE_INT_FRACTION;

	value->type = type;
	mp_value_set_fraction_data(&value->v_fraction_range.min, base_type, args);
	mp_value_set_fraction_data(&value->v_fraction_range.max, base_type, args);
	mp_value_set_fraction_data(&value->v_fraction_range.step, base_type, args);
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
	return frac->v_fraction.num.v_int;
}

int mp_value_get_fraction_denominator(const struct mp_value *frac)
{
	return frac->v_fraction.denom.v_int;
}

static inline enum mp_value_type fraction_subtype(const struct mp_value *fraction_range)
{
	return fraction_range->type == MP_TYPE_UINT_FRACTION_RANGE ? MP_TYPE_UINT_FRACTION
								   : MP_TYPE_INT_FRACTION;
}

/*
 * Internal helper: compare two raw fraction_data payloads with their
 * respective subtypes. Internal callers must use this rather than going
 * through the public mp_value_get_fraction_range_* accessors, which share
 * a static view buffer and are safe only when the returned pointer is
 * consumed before the next call.
 */
static int compare_fraction_data(enum mp_value_type t1,
				 const struct mp_value_fraction_data *f1,
				 enum mp_value_type t2,
				 const struct mp_value_fraction_data *f2)
{
	if (t1 != t2) {
		return MP_VALUE_COMPARE_FAILED;
	}

	if (t1 == MP_TYPE_INT_FRACTION) {
		return mp_fraction_compare(f1->num.v_int, f1->denom.v_int, f2->num.v_int,
					   f2->denom.v_int);
	}

	if (t1 == MP_TYPE_UINT_FRACTION) {
		return mp_fraction_compare(f1->num.v_uint, f1->denom.v_uint, f2->num.v_uint,
					   f2->denom.v_uint);
	}

	return MP_VALUE_COMPARE_FAILED;
}

/*
 * Public fraction range accessors. The returned pointer aliases an
 * internal static buffer that is reused across calls — the caller must
 * consume the returned value (e.g., duplicate or read its payload)
 * before calling another fraction_range accessor.
 */
static struct mp_value g_fraction_range_view;

static const struct mp_value *fraction_range_view(const struct mp_value *fraction_range,
						  const struct mp_value_fraction_data *frac)
{
	g_fraction_range_view.type = fraction_subtype(fraction_range);
	g_fraction_range_view.v_fraction = *frac;
	return &g_fraction_range_view;
}

const struct mp_value *mp_value_get_fraction_range_min(const struct mp_value *fraction_range)
{
	return fraction_range_view(fraction_range, &fraction_range->v_fraction_range.min);
}

const struct mp_value *mp_value_get_fraction_range_max(const struct mp_value *fraction_range)
{
	return fraction_range_view(fraction_range, &fraction_range->v_fraction_range.max);
}

const struct mp_value *mp_value_get_fraction_range_step(const struct mp_value *fraction_range)
{
	return fraction_range_view(fraction_range, &fraction_range->v_fraction_range.step);
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

	if (k_mem_slab_alloc(&mp_value_slab, (void **)&value, K_NO_WAIT) != 0) {
		LOG_ERR("mp_value pool exhausted (CONFIG_MP_VALUE_POOL_SIZE=%d)",
			CONFIG_MP_VALUE_POOL_SIZE);
		return NULL;
	}

	memset(value, 0, sizeof(*value));
	value->type = type;
	if (type == MP_TYPE_LIST) {
		sys_slist_init(&value->v_list);
	}

	return value;
}

void mp_value_destroy(struct mp_value *value)
{
	struct mp_value *item, *tmp;

	if (value == NULL) {
		return;
	}

	if (value->type == MP_TYPE_LIST) {
		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&value->v_list, item, tmp, node) {
			mp_value_destroy(item);
		}
		sys_slist_init(&value->v_list);
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

	if (value == NULL) {
		return NULL;
	}

	mp_value_set_va_list(value, type, args);

	return value;
}

static void mp_value_copy(struct mp_value *dst, const struct mp_value *src)
{
	if (src->type == MP_TYPE_LIST) {
		struct mp_value *item;

		SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&src->v_list, item, node) {
			mp_value_list_append(dst, mp_value_duplicate(item));
		}
	} else if (src->type == MP_TYPE_OBJECT) {
		dst->v_obj = src->v_obj;
		mp_object_ref(dst->v_obj);
	} else {
		/* Copy the payload but preserve dst's link state. */
		dst->type = src->type;
		dst->field_id = src->field_id;
		dst->v_fraction_range = src->v_fraction_range;
	}
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

	mp_value_copy(dup_value, value);

	return dup_value;
}

void mp_value_list_append(struct mp_value *list, struct mp_value *append_value)
{
	__ASSERT_NO_MSG(append_value != NULL && list != NULL);
	__ASSERT_NO_MSG(list->type == MP_TYPE_LIST);
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
	return range->v_range.min.v_int;
}

int mp_value_get_int_range_max(const struct mp_value *range)
{
	return range->v_range.max.v_int;
}

int mp_value_get_int_range_step(const struct mp_value *range)
{
	return range->v_range.step.v_int;
}

uint32_t mp_value_get_uint_range_min(const struct mp_value *range)
{
	return range->v_range.min.v_uint;
}

uint32_t mp_value_get_uint_range_max(const struct mp_value *range)
{
	return range->v_range.max.v_uint;
}

uint32_t mp_value_get_uint_range_step(const struct mp_value *range)
{
	return range->v_range.step.v_uint;
}

struct mp_object *mp_value_get_object(struct mp_value *value)
{
	return value ? value->v_obj : NULL;
}

int mp_value_compare_fraction(const struct mp_value *frac1, const struct mp_value *frac2)
{
	if (frac1->type == MP_TYPE_INT_FRACTION && frac2->type == MP_TYPE_INT_FRACTION) {
		return mp_fraction_compare(frac1->v_fraction.num.v_int,
					   frac1->v_fraction.denom.v_int,
					   frac2->v_fraction.num.v_int,
					   frac2->v_fraction.denom.v_int);
	}

	if (frac1->type == MP_TYPE_UINT_FRACTION && frac2->type == MP_TYPE_UINT_FRACTION) {
		return mp_fraction_compare(frac1->v_fraction.num.v_uint,
					   frac1->v_fraction.denom.v_uint,
					   frac2->v_fraction.num.v_uint,
					   frac2->v_fraction.denom.v_uint);
	}

	return MP_VALUE_COMPARE_FAILED;
}

static int mp_value_list_compare(const struct mp_value *list1, const struct mp_value *list2);

int mp_value_compare(const struct mp_value *val1, const struct mp_value *val2)
{
	bool is_equal;

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
		is_equal = (val1->v_range.min.v_uint == val2->v_range.min.v_uint &&
			    val1->v_range.max.v_uint == val2->v_range.max.v_uint &&
			    val1->v_range.step.v_uint == val2->v_range.step.v_uint);
		return is_equal ? MP_VALUE_EQUAL : MP_VALUE_UNORDERED;
	case MP_TYPE_INT_FRACTION_RANGE:
	case MP_TYPE_UINT_FRACTION_RANGE: {
		enum mp_value_type ftype = fraction_subtype(val1);

		is_equal = compare_fraction_data(ftype, &val1->v_fraction_range.min, ftype,
						 &val2->v_fraction_range.min) == MP_VALUE_EQUAL &&
			   compare_fraction_data(ftype, &val1->v_fraction_range.max, ftype,
						 &val2->v_fraction_range.max) == MP_VALUE_EQUAL &&
			   compare_fraction_data(ftype, &val1->v_fraction_range.step, ftype,
						 &val2->v_fraction_range.step) == MP_VALUE_EQUAL;
		return is_equal ? MP_VALUE_EQUAL : MP_VALUE_UNORDERED;
	}
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
				break;
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

static bool int_range_overlap(const struct mp_value *r1, const struct mp_value *r2, bool is_signed)
{
	if (is_signed) {
		return !(r1->v_range.min.v_int > r2->v_range.max.v_int ||
			 r2->v_range.min.v_int > r1->v_range.max.v_int);
	}
	return !(r1->v_range.min.v_uint > r2->v_range.max.v_uint ||
		 r2->v_range.min.v_uint > r1->v_range.max.v_uint);
}

struct mp_value *mp_value_intersect_int_range(const struct mp_value *ref_val,
					      const struct mp_value *compare_val)
{
	if (compare_val->type == MP_TYPE_INT_RANGE && ref_val->type == MP_TYPE_INT_RANGE) {
		if (!int_range_overlap(ref_val, compare_val, true)) {
			return NULL;
		}
		return mp_value_new(MP_TYPE_INT_RANGE,
				    MAX(ref_val->v_range.min.v_int, compare_val->v_range.min.v_int),
				    MIN(ref_val->v_range.max.v_int, compare_val->v_range.max.v_int),
				    sys_gcd(ref_val->v_range.step.v_int,
					    compare_val->v_range.step.v_int),
				    NULL);
	}

	if (compare_val->type == MP_TYPE_UINT_RANGE && ref_val->type == MP_TYPE_UINT_RANGE) {
		if (!int_range_overlap(ref_val, compare_val, false)) {
			return NULL;
		}
		return mp_value_new(MP_TYPE_UINT_RANGE,
				    MAX(ref_val->v_range.min.v_uint, compare_val->v_range.min.v_uint),
				    MIN(ref_val->v_range.max.v_uint, compare_val->v_range.max.v_uint),
				    sys_gcd(ref_val->v_range.step.v_uint,
					    compare_val->v_range.step.v_uint),
				    NULL);
	}

	if (ref_val->type == MP_TYPE_INT_RANGE && compare_val->type == MP_TYPE_INT &&
	    IN_RANGE(compare_val->v_int, ref_val->v_range.min.v_int, ref_val->v_range.max.v_int)) {
		return mp_value_new(MP_TYPE_INT, compare_val->v_int, NULL);
	}

	if (ref_val->type == MP_TYPE_UINT_RANGE && compare_val->type == MP_TYPE_UINT &&
	    IN_RANGE(compare_val->v_uint, ref_val->v_range.min.v_uint,
		     ref_val->v_range.max.v_uint)) {
		return mp_value_new(MP_TYPE_UINT, compare_val->v_uint, NULL);
	}

	return NULL;
}

static bool fraction_range_overlap(const struct mp_value *ref, const struct mp_value *cmp)
{
	enum mp_value_type ft_ref = fraction_subtype(ref);
	enum mp_value_type ft_cmp = fraction_subtype(cmp);

	return !(compare_fraction_data(ft_ref, &ref->v_fraction_range.min, ft_cmp,
				       &cmp->v_fraction_range.max) == MP_VALUE_GREATER_THAN ||
		 compare_fraction_data(ft_ref, &ref->v_fraction_range.max, ft_cmp,
				       &cmp->v_fraction_range.min) == MP_VALUE_LESS_THAN);
}

static bool fraction_in_range(const struct mp_value *frac, const struct mp_value *range)
{
	enum mp_value_type ft_range = fraction_subtype(range);

	return !(compare_fraction_data(frac->type, &frac->v_fraction, ft_range,
				       &range->v_fraction_range.min) == MP_VALUE_LESS_THAN ||
		 compare_fraction_data(frac->type, &frac->v_fraction, ft_range,
				       &range->v_fraction_range.max) == MP_VALUE_GREATER_THAN);
}

struct mp_value *mp_value_intersect_fraction_range(const struct mp_value *ref_val,
						   const struct mp_value *compare_val)
{
	struct mp_value *intersect_value;
	enum mp_value_type f_type = fraction_subtype(ref_val);

	if ((compare_val->type == MP_TYPE_UINT_FRACTION_RANGE ||
	     compare_val->type == MP_TYPE_INT_FRACTION_RANGE) &&
	    fraction_range_overlap(ref_val, compare_val)) {
		const struct mp_value_fraction_data *new_min;
		const struct mp_value_fraction_data *new_max;
		int cmp;

		intersect_value = mp_value_new_empty(ref_val->type);
		if (intersect_value == NULL) {
			return NULL;
		}

		/* min = max(ref.min, cmp.min) */
		cmp = compare_fraction_data(f_type, &ref_val->v_fraction_range.min, f_type,
					    &compare_val->v_fraction_range.min);
		new_min = (cmp == MP_VALUE_GREATER_THAN) ? &ref_val->v_fraction_range.min
							 : &compare_val->v_fraction_range.min;

		/* max = min(ref.max, cmp.max) */
		cmp = compare_fraction_data(f_type, &ref_val->v_fraction_range.max, f_type,
					    &compare_val->v_fraction_range.max);
		new_max = (cmp == MP_VALUE_LESS_THAN) ? &ref_val->v_fraction_range.max
						      : &compare_val->v_fraction_range.max;

		intersect_value->v_fraction_range.min = *new_min;
		intersect_value->v_fraction_range.max = *new_max;
		if (f_type == MP_TYPE_INT_FRACTION) {
			intersect_value->v_fraction_range.step.num.v_int =
				sys_gcd(ref_val->v_fraction_range.step.num.v_int,
					compare_val->v_fraction_range.step.num.v_int);
			intersect_value->v_fraction_range.step.denom.v_int =
				sys_lcm(ref_val->v_fraction_range.step.denom.v_int,
					compare_val->v_fraction_range.step.denom.v_int);
		} else {
			intersect_value->v_fraction_range.step.num.v_uint =
				sys_gcd(ref_val->v_fraction_range.step.num.v_uint,
					compare_val->v_fraction_range.step.num.v_uint);
			intersect_value->v_fraction_range.step.denom.v_uint =
				sys_lcm(ref_val->v_fraction_range.step.denom.v_uint,
					compare_val->v_fraction_range.step.denom.v_uint);
		}
		return intersect_value;
	}

	if ((compare_val->type == MP_TYPE_INT_FRACTION ||
	     compare_val->type == MP_TYPE_UINT_FRACTION) &&
	    fraction_in_range(compare_val, ref_val)) {
		return mp_value_new(f_type, compare_val->v_fraction.num.v_uint,
				    compare_val->v_fraction.denom.v_uint, NULL);
	}

	return NULL;
}

struct mp_value *mp_value_intersect_list(const struct mp_value *list,
					 const struct mp_value *compare_val)
{
	struct mp_value *intersect_value = NULL;
	struct mp_value *intersect_list = NULL;
	struct mp_value *item1, *item2;

	if (list == NULL || compare_val == NULL || compare_val->type == MP_TYPE_NONE) {
		return NULL;
	}

	SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&list->v_list, item1, node) {
		intersect_value = NULL;
		switch (compare_val->type) {
		case MP_TYPE_BOOLEAN:
		case MP_TYPE_ENUM:
		case MP_TYPE_INT:
		case MP_TYPE_UINT:
		case MP_TYPE_UINT_FRACTION:
		case MP_TYPE_INT_FRACTION:
		case MP_TYPE_STRING:
			if (mp_value_compare(compare_val, item1) == MP_VALUE_EQUAL) {
				intersect_value = mp_value_duplicate(compare_val);
			}
			break;
		case MP_TYPE_INT_RANGE:
		case MP_TYPE_UINT_RANGE:
			intersect_value = mp_value_intersect_int_range(compare_val, item1);
			break;
		case MP_TYPE_UINT_FRACTION_RANGE:
		case MP_TYPE_INT_FRACTION_RANGE:
			intersect_value = mp_value_intersect_fraction_range(compare_val, item1);
			break;
		case MP_TYPE_LIST:
			SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&compare_val->v_list, item2,
						     node) {
				if (mp_value_compare(item1, item2) == MP_VALUE_EQUAL) {
					intersect_value = mp_value_duplicate(item2);
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

	if (!mp_value_can_intersect(val1, val2)) {
		return NULL;
	}

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
	printk("[%d, %d, %d]", value->v_range.min.v_int, value->v_range.max.v_int,
	       value->v_range.step.v_int);
}

static inline void mp_value_print_uint_range(const struct mp_value *value)
{
	printk("[%u, %u, %u]", value->v_range.min.v_uint, value->v_range.max.v_uint,
	       value->v_range.step.v_uint);
}

static inline void mp_value_print_fraction(const struct mp_value *value)
{
	if (value->type == MP_TYPE_UINT_FRACTION) {
		printk("%u/%u", value->v_fraction.num.v_uint, value->v_fraction.denom.v_uint);
	} else {
		printk("%d/%d", value->v_fraction.num.v_int, value->v_fraction.denom.v_int);
	}
}

static inline void mp_value_print_fraction_range(const struct mp_value *value)
{
	int ftype = (value->type == MP_TYPE_UINT_FRACTION_RANGE) ? MP_TYPE_UINT_FRACTION
								 : MP_TYPE_INT_FRACTION;
	struct mp_value view;

	view.type = ftype;
	printk("[");
	view.v_fraction = value->v_fraction_range.min;
	mp_value_print_fraction(&view);
	printk(",");
	view.v_fraction = value->v_fraction_range.max;
	mp_value_print_fraction(&view);
	printk(",");
	view.v_fraction = value->v_fraction_range.step;
	mp_value_print_fraction(&view);
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

	mp_value_print_table[value->type](value);

	if (new_line) {
		printk("\n");
	}
}
