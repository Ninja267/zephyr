/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/mp/core/mp_bus.h>

enum mp_bus_sync_reply {
	MP_BUS_DROP = 0,
	MP_BUS_PASS = 1,
};

/**
 * Sync handler for the bus.
 * This function is called when a message is posted to the bus.
 * It delivers the message to the correct listener based on the filter type.
 * If a listener consumes the message, it returns MP_BUS_DROP,
 * otherwise it returns MP_BUS_PASS.
 *

 * @param bus: a struct mp_bus to handle the message
 * @param message: the message to handle
 */
static enum mp_bus_sync_reply mp_bus_sync_handler(struct mp_bus *bus, struct mp_message *message)
{

	struct mp_bus_sync_listener *listener;
	bool ret = false;

	/* Deliver the message to the correct listener */
	SYS_SLIST_FOR_EACH_CONTAINER(&bus->sync_listeners, listener, node) {
		if (message->type & listener->filter_type) {
			ret |= listener->cb(message, listener->user_data);
		}
	}

	return ret ? MP_BUS_DROP : MP_BUS_PASS;
}

bool mp_bus_post(struct mp_bus *bus, struct mp_message *message)
{

	enum mp_bus_sync_reply reply = MP_BUS_PASS;

	if (bus == NULL || message == NULL) {
		return false;
	}

	/* Step 1: Notify the sync handler first if any */
	if (!sys_slist_is_empty(&bus->sync_listeners)) {
		reply = mp_bus_sync_handler(bus, message);
	}

	/* Step 2: Destroy the message if a listener consumed it */
	if (reply == MP_BUS_DROP) {
		mp_message_destroy(message);
		return true;
	}

	/* Step 3: Queue the message pointer, the payload is not copied.
	 * The queue is sized to the message pool so this cannot fail, but
	 * never lose a pooled message should the sizes ever diverge.
	 */
	if (k_msgq_put(&bus->msgq, &message, K_NO_WAIT) != 0) {
		mp_message_destroy(message);
		return false;
	}

	return true;
}

struct mp_message *mp_bus_pop_msg(struct mp_bus *bus, enum mp_message_type type)
{
	__ASSERT_NO_MSG(bus != NULL);

	struct mp_message *message;

	while (k_msgq_get(&bus->msgq, &message, K_FOREVER) == 0) {
		if (message->type & type) {
			return message;
		}

		/* Discard unmatched message */
		mp_message_destroy(message);
	}

	return NULL;
}

struct mp_message *mp_bus_pop(struct mp_bus *bus)
{
	return mp_bus_pop_msg(bus, MP_MESSAGE_ANY);
}

struct mp_message *mp_bus_peek(struct mp_bus *bus)
{
	struct mp_message *message;

	if (bus == NULL || k_msgq_peek(&bus->msgq, &message) != 0) {
		return NULL;
	}

	return message;
}
void mp_bus_flush(struct mp_bus *bus)
{
	struct mp_message *message;

	__ASSERT_NO_MSG(bus != NULL);

	/** Drain the queue and free all messages. k_msgq_purge() would
	 * discard the pointers without destroying the pooled messages.
	 */
	while (k_msgq_get(&bus->msgq, &message, K_NO_WAIT) == 0) {
		mp_message_destroy(message);
	}
}

void mp_bus_add_sync_listener(struct mp_bus *bus, struct mp_bus_sync_listener *listener,
			      callback_fn cb, enum mp_message_type type, void *user_data)
{
	__ASSERT_NO_MSG(bus != NULL && listener != NULL);
	listener->cb = cb;
	listener->filter_type = type;
	listener->user_data = user_data;
	sys_slist_append(&bus->sync_listeners, &listener->node);
}

void mp_bus_remove_sync_listener(struct mp_bus *bus, struct mp_bus_sync_listener *listener)
{
	__ASSERT_NO_MSG(bus != NULL && listener != NULL);
	sys_slist_find_and_remove(&bus->sync_listeners, &listener->node);
}
