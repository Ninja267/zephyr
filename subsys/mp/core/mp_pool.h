/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mp
 * @brief Fixed-size object pools for the MP core.
 *
 * The MP core objects (caps, structures, values, ...) are small, short-lived
 * and allocated/freed frequently during caps negotiation. On an embedded
 * target, servicing those from the system heap is expensive: it is
 * non-deterministic, fragments memory and carries per-allocation overhead.
 *
 * This header provides a thin wrapper around @ref k_mem_slab so each object
 * type is served from a statically sized pool. Allocation is O(1), bounded at
 * build time and free of fragmentation.
 */

#ifndef ZEPHYR_SUBSYS_MP_CORE_MP_POOL_H_
#define ZEPHYR_SUBSYS_MP_CORE_MP_POOL_H_

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/**
 * @brief Statically define a fixed-size object pool for @p type.
 *
 * @param name  Symbol name of the pool (a @ref k_mem_slab).
 * @param type  Object type stored in the pool.
 * @param count Number of preallocated objects.
 */
#define MP_POOL_DEFINE(name, type, count)                                                          \
	K_MEM_SLAB_DEFINE_STATIC(name, ROUND_UP(sizeof(type), sizeof(void *)), count, sizeof(void *))

/**
 * @brief Statically define a fixed-size object pool of @p size byte blocks.
 *
 * Used when a single pool must accommodate several related types (e.g. the
 * value variants), in which case @p size is the size of the largest one.
 *
 * @param name  Symbol name of the pool (a @ref k_mem_slab).
 * @param size  Block size in bytes.
 * @param count Number of preallocated blocks.
 */
#define MP_POOL_DEFINE_SZ(name, size, count)                                                       \
	K_MEM_SLAB_DEFINE_STATIC(name, ROUND_UP(size, sizeof(void *)), count, sizeof(void *))

/**
 * @brief Allocate one object from a pool.
 *
 * @param pool Pool to allocate from.
 * @return Pointer to the block, or NULL if the pool is exhausted.
 */
static inline void *mp_pool_alloc(struct k_mem_slab *pool)
{
	void *block;

	if (k_mem_slab_alloc(pool, &block, K_NO_WAIT) != 0) {
		return NULL;
	}

	return block;
}

/**
 * @brief Allocate one zero-initialized object from a pool.
 *
 * @param pool Pool to allocate from.
 * @param size Number of leading bytes to clear.
 * @return Pointer to the zeroed block, or NULL if the pool is exhausted.
 */
static inline void *mp_pool_calloc(struct k_mem_slab *pool, size_t size)
{
	void *block = mp_pool_alloc(pool);

	if (block != NULL) {
		memset(block, 0, size);
	}

	return block;
}

/**
 * @brief Return an object to its pool.
 *
 * @param pool  Pool the block was allocated from.
 * @param block Block to free (NULL is ignored).
 */
static inline void mp_pool_free(struct k_mem_slab *pool, void *block)
{
	if (block != NULL) {
		k_mem_slab_free(pool, block);
	}
}

#endif /* ZEPHYR_SUBSYS_MP_CORE_MP_POOL_H_ */
