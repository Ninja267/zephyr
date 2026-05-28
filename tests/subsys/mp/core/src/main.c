/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include <zephyr/mp/core/mp_caps.h>
#include <zephyr/mp/core/mp_structure.h>
#include <zephyr/mp/core/mp_value.h>

/* ------------------------------------------------------------------------- */
/* mp_value                                                                  */
/* ------------------------------------------------------------------------- */

ZTEST(mp_core, test_value_scalars)
{
	struct mp_value *v;

	v = mp_value_new(MP_TYPE_INT, -42);
	zassert_not_null(v);
	zassert_equal(mp_value_get_type(v), MP_TYPE_INT);
	zassert_equal(mp_value_get_int(v), -42);
	zassert_true(mp_value_is_primitive(v));
	mp_value_destroy(v);

	v = mp_value_new(MP_TYPE_UINT, 4242u);
	zassert_not_null(v);
	zassert_equal(mp_value_get_uint(v), 4242u);
	mp_value_destroy(v);

	v = mp_value_new(MP_TYPE_BOOLEAN, true);
	zassert_not_null(v);
	zassert_true(mp_value_get_boolean(v));
	mp_value_destroy(v);

	v = mp_value_new(MP_TYPE_STRING, "RGB565");
	zassert_not_null(v);
	zassert_mem_equal(mp_value_get_string(v), "RGB565", sizeof("RGB565"));
	mp_value_destroy(v);
}

ZTEST(mp_core, test_value_fraction_reduces)
{
	struct mp_value *a, *b;

	/* 30/2 must be stored reduced as 15/1. */
	a = mp_value_new(MP_TYPE_INT_FRACTION, 30, 2);
	zassert_not_null(a);
	zassert_equal(mp_value_get_fraction_numerator(a), 15);
	zassert_equal(mp_value_get_fraction_denominator(a), 1);

	b = mp_value_new(MP_TYPE_INT_FRACTION, 15, 1);
	zassert_not_null(b);
	zassert_equal(mp_value_compare(a, b), MP_VALUE_EQUAL);
	zassert_equal(mp_value_compare_fraction(a, b), MP_VALUE_EQUAL);

	mp_value_destroy(a);
	mp_value_destroy(b);
}

ZTEST(mp_core, test_value_int_range_intersect)
{
	struct mp_value *r1, *r2, *single, *res;

	r1 = mp_value_new(MP_TYPE_INT_RANGE, 100, 200, 10);
	zassert_not_null(r1);
	zassert_equal(mp_value_get_int_range_min(r1), 100);
	zassert_equal(mp_value_get_int_range_max(r1), 200);
	zassert_equal(mp_value_get_int_range_step(r1), 10);

	/* Overlapping ranges: [100,200]^[150,300] = [150,200]. */
	r2 = mp_value_new(MP_TYPE_INT_RANGE, 150, 300, 10);
	res = mp_value_intersect(r1, r2);
	zassert_not_null(res);
	zassert_equal(mp_value_get_type(res), MP_TYPE_INT_RANGE);
	zassert_equal(mp_value_get_int_range_min(res), 150);
	zassert_equal(mp_value_get_int_range_max(res), 200);
	mp_value_destroy(res);
	mp_value_destroy(r2);

	/* A single value inside the range collapses to that value. */
	single = mp_value_new(MP_TYPE_INT, 150);
	res = mp_value_intersect(r1, single);
	zassert_not_null(res);
	zassert_equal(mp_value_get_type(res), MP_TYPE_INT);
	zassert_equal(mp_value_get_int(res), 150);
	mp_value_destroy(res);
	mp_value_destroy(single);

	/* A single value outside the range does not intersect. */
	single = mp_value_new(MP_TYPE_INT, 999);
	zassert_is_null(mp_value_intersect(r1, single));
	mp_value_destroy(single);

	mp_value_destroy(r1);
}

ZTEST(mp_core, test_value_list)
{
	struct mp_value *list, *dup, *single, *res;

	list = mp_value_new_empty(MP_TYPE_LIST);
	zassert_not_null(list);
	zassert_true(mp_value_list_is_empty(list));

	mp_value_list_append(list, mp_value_new(MP_TYPE_UINT, 1u));
	mp_value_list_append(list, mp_value_new(MP_TYPE_UINT, 2u));
	mp_value_list_append(list, mp_value_new(MP_TYPE_UINT, 3u));

	zassert_equal(mp_value_list_get_size(list), 3);
	zassert_false(mp_value_list_is_empty(list));
	zassert_equal(mp_value_get_uint(mp_value_list_get(list, 0)), 1u);
	zassert_equal(mp_value_get_uint(mp_value_list_get(list, 2)), 3u);
	zassert_is_null(mp_value_list_get(list, 7));

	/* Duplicate must deep-copy the elements. */
	dup = mp_value_duplicate(list);
	zassert_not_null(dup);
	zassert_equal(mp_value_list_get_size(dup), 3);
	zassert_equal(mp_value_compare(list, dup), MP_VALUE_EQUAL);
	mp_value_destroy(dup);

	/* Intersect list with a member value -> a one-element list. */
	single = mp_value_new(MP_TYPE_UINT, 2u);
	res = mp_value_intersect(list, single);
	zassert_not_null(res);
	zassert_equal(mp_value_get_type(res), MP_TYPE_LIST);
	zassert_equal(mp_value_list_get_size(res), 1);
	zassert_equal(mp_value_get_uint(mp_value_list_get(res, 0)), 2u);
	mp_value_destroy(res);
	mp_value_destroy(single);

	mp_value_destroy(list);
}

ZTEST(mp_core, test_value_fraction_range_intersect)
{
	struct mp_value *range, *frac, *res;

	/* framerate in [15/1, 60/1] step 1/1. */
	range = mp_value_new(MP_TYPE_INT_FRACTION_RANGE, 15, 1, 60, 1, 1, 1);
	zassert_not_null(range);

	/* 30/1 lies in the range -> collapses to the fraction. */
	frac = mp_value_new(MP_TYPE_INT_FRACTION, 30, 1);
	res = mp_value_intersect(range, frac);
	zassert_not_null(res);
	zassert_equal(mp_value_get_type(res), MP_TYPE_INT_FRACTION);
	zassert_equal(mp_value_get_fraction_numerator(res), 30);
	zassert_equal(mp_value_get_fraction_denominator(res), 1);
	mp_value_destroy(res);
	mp_value_destroy(frac);

	/* 90/1 lies outside the range. */
	frac = mp_value_new(MP_TYPE_INT_FRACTION, 90, 1);
	zassert_is_null(mp_value_intersect(range, frac));
	mp_value_destroy(frac);

	mp_value_destroy(range);
}

/* ------------------------------------------------------------------------- */
/* mp_structure                                                              */
/* ------------------------------------------------------------------------- */

ZTEST(mp_core, test_structure_basics)
{
	struct mp_structure *s, *dup;

	s = mp_structure_new(MP_MEDIA_VIDEO, MP_CAPS_IMAGE_WIDTH, MP_TYPE_INT, 1920,
			     MP_CAPS_IMAGE_HEIGHT, MP_TYPE_INT, 1080, MP_CAPS_END);
	zassert_not_null(s);
	zassert_equal(mp_value_get_int(mp_structure_get_value(s, MP_CAPS_IMAGE_WIDTH)), 1920);
	zassert_equal(mp_value_get_int(mp_structure_get_value(s, MP_CAPS_IMAGE_HEIGHT)), 1080);
	zassert_is_null(mp_structure_get_value(s, MP_CAPS_FRAME_RATE));
	zassert_true(mp_structure_is_fixed(s));

	dup = mp_structure_duplicate(s);
	zassert_not_null(dup);
	zassert_equal(mp_value_get_int(mp_structure_get_value(dup, MP_CAPS_IMAGE_WIDTH)), 1920);
	mp_structure_destroy(dup);

	zassert_true(mp_structure_remove_field(s, MP_CAPS_IMAGE_WIDTH));
	zassert_is_null(mp_structure_get_value(s, MP_CAPS_IMAGE_WIDTH));
	zassert_false(mp_structure_remove_field(s, MP_CAPS_IMAGE_WIDTH));

	mp_structure_destroy(s);
}

ZTEST(mp_core, test_structure_intersect_and_fixate)
{
	struct mp_structure *s1, *s2, *inter, *fixed;

	/* width range vs a single width that falls in the range. */
	s1 = mp_structure_new(MP_MEDIA_VIDEO, MP_CAPS_IMAGE_WIDTH, MP_TYPE_INT_RANGE, 640, 1920, 1,
			      MP_CAPS_END);
	s2 = mp_structure_new(MP_MEDIA_VIDEO, MP_CAPS_IMAGE_WIDTH, MP_TYPE_INT, 1280, MP_CAPS_END);
	zassert_true(mp_structure_can_intersect(s1, s2));

	inter = mp_structure_intersect(s1, s2);
	zassert_not_null(inter);
	zassert_equal(mp_value_get_int(mp_structure_get_value(inter, MP_CAPS_IMAGE_WIDTH)), 1280);
	zassert_true(mp_structure_is_fixed(inter));
	mp_structure_destroy(inter);
	mp_structure_destroy(s2);

	/* A range-only structure is not fixed but can be fixated to its min. */
	zassert_false(mp_structure_is_fixed(s1));
	fixed = mp_structure_fixate(s1);
	zassert_not_null(fixed);
	zassert_true(mp_structure_is_fixed(fixed));
	zassert_equal(mp_value_get_int(mp_structure_get_value(fixed, MP_CAPS_IMAGE_WIDTH)), 640);
	mp_structure_destroy(fixed);

	mp_structure_destroy(s1);
}

/* ------------------------------------------------------------------------- */
/* mp_caps                                                                   */
/* ------------------------------------------------------------------------- */

ZTEST(mp_core, test_caps_any_empty)
{
	struct mp_caps *any = mp_caps_new_any();

	zassert_not_null(any);
	zassert_true(mp_caps_is_any(any));
	zassert_false(mp_caps_is_empty(any));
	zassert_false(mp_caps_is_fixed(any));
	mp_caps_unref(any);
}

ZTEST(mp_core, test_caps_intersect_fixate)
{
	struct mp_caps *caps1, *caps2, *inter, *fixed, *empty;

	caps1 = mp_caps_new(MP_MEDIA_VIDEO, MP_CAPS_IMAGE_WIDTH, MP_TYPE_INT_RANGE, 100, 200, 10,
			    MP_CAPS_END);
	zassert_not_null(caps1);
	zassert_false(mp_caps_is_fixed(caps1));

	caps2 = mp_caps_new(MP_MEDIA_VIDEO, MP_CAPS_IMAGE_WIDTH, MP_TYPE_INT, 150, MP_CAPS_END);
	zassert_not_null(caps2);

	/* The single value lies in the range -> fixed intersection of 150. */
	inter = mp_caps_intersect(caps1, caps2);
	zassert_not_null(inter);
	zassert_false(mp_caps_is_empty(inter));
	zassert_true(mp_caps_is_fixed(inter));
	zassert_equal(mp_value_get_int(
			      mp_structure_get_value(mp_caps_get_structure(inter, 0),
						     MP_CAPS_IMAGE_WIDTH)),
		      150);
	mp_caps_unref(inter);
	mp_caps_unref(caps2);

	/* A value outside the range yields an empty intersection. */
	caps2 = mp_caps_new(MP_MEDIA_VIDEO, MP_CAPS_IMAGE_WIDTH, MP_TYPE_INT, 999, MP_CAPS_END);
	empty = mp_caps_intersect(caps1, caps2);
	zassert_not_null(empty);
	zassert_true(mp_caps_is_empty(empty));
	mp_caps_unref(empty);
	mp_caps_unref(caps2);

	/* Intersection with ANY duplicates the other caps. */
	struct mp_caps *any = mp_caps_new_any();

	inter = mp_caps_intersect(any, caps1);
	zassert_not_null(inter);
	zassert_false(mp_caps_is_any(inter));
	mp_caps_unref(inter);
	mp_caps_unref(any);

	/* Fixate the range caps down to its minimum. */
	fixed = mp_caps_fixate(caps1);
	zassert_not_null(fixed);
	zassert_true(mp_caps_is_fixed(fixed));
	zassert_equal(mp_value_get_int(
			      mp_structure_get_value(mp_caps_get_structure(fixed, 0),
						     MP_CAPS_IMAGE_WIDTH)),
		      100);
	mp_caps_unref(fixed);

	mp_caps_unref(caps1);
}

/* ------------------------------------------------------------------------- */
/* Pool accounting: repeatedly allocating and freeing must not leak slots.   */
/* ------------------------------------------------------------------------- */

ZTEST(mp_core, test_pools_do_not_leak)
{
	/* Far more iterations than any pool size: a leaked slot would make a
	 * later allocation fail and the assert below would trip.
	 */
	for (int i = 0; i < 1000; i++) {
		struct mp_value *v = mp_value_new(MP_TYPE_INT, i);

		zassert_not_null(v, "value pool leaked at iteration %d", i);
		mp_value_destroy(v);
	}

	for (int i = 0; i < 1000; i++) {
		struct mp_caps *caps = mp_caps_new(MP_MEDIA_VIDEO, MP_CAPS_IMAGE_WIDTH, MP_TYPE_INT,
						   i, MP_CAPS_IMAGE_HEIGHT, MP_TYPE_INT, i + 1,
						   MP_CAPS_END);

		zassert_not_null(caps, "caps/structure/field pool leaked at iteration %d", i);
		zassert_equal(mp_value_get_int(mp_structure_get_value(
				      mp_caps_get_structure(caps, 0), MP_CAPS_IMAGE_HEIGHT)),
			      i + 1);
		mp_caps_unref(caps);
	}
}

ZTEST_SUITE(mp_core, NULL, NULL, NULL, NULL, NULL);
