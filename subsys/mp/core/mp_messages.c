/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include <zephyr/mp/core/mp_messages.h>
#include <zephyr/mp/core/mp_structure.h>

#include "mp_pool.h"

MP_POOL_DEFINE(mp_message_pool, struct mp_message, CONFIG_MP_MESSAGE_POOL_SIZE);

/* Messages are created from pipeline threads, so the counter is atomic. */
static atomic_t seq_id = ATOMIC_INIT(1);

struct mp_message *mp_message_new(enum mp_message_type type, struct mp_object *src,
				  struct mp_structure *data)
{
	struct mp_message *msg = mp_pool_alloc(&mp_message_pool);

	if (msg == NULL) {
		return NULL;
	}

	msg->type = type;
	msg->src = src;
	msg->timestamp = k_uptime_get_32();
	msg->seq_id = (uint32_t)atomic_inc(&seq_id);
	msg->data = data;

	return msg;
}

void mp_message_destroy(struct mp_message *msg)
{
	if (msg == NULL) {
		return;
	}

	if (msg->data != NULL) {
		mp_structure_destroy(msg->data);
	}

	mp_pool_free(&mp_message_pool, msg);
}
