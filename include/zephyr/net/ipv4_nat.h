/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief IPv4 NAT and iptable interface for Zephyr networking stack.
 *
 * This header defines the data structures and APIs used for
 * IPv4 NAT (Network Address Translation) and rule-based iptable
 * manipulation in the Zephyr networking subsystem.
 */

#ifndef ZEPHYR_INCLUDE_NET_IPV4_NAT_H_
#define ZEPHYR_INCLUDE_NET_IPV4_NAT_H_

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <stdbool.h>

#include <zephyr/net/net_core.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_pkt.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parameters to define an iptable rule.
 *
 * Used with table rule management APIs to specify matching criteria,
 * input/output interfaces, protocols, and timeout values.
 */
struct iptable_rule_params {
	/** Input interface index. */
	int input_iface_idx;
	/** Output interface index. */
	int output_iface_idx;
	/** Source IPv4 address. */
	uint8_t src[NET_IPV4_ADDR_SIZE];
	/** Source IPv4 mask. */
	uint8_t src_mask[NET_IPV4_ADDR_SIZE];
	/** Destination IPv4 address. */
	uint8_t dst[NET_IPV4_ADDR_SIZE];
	/** Destination IPv4 mask. */
	uint8_t dst_mask[NET_IPV4_ADDR_SIZE];
	/** Protocol number (e.g., IPPROTO_TCP, IPPROTO_UDP). */
	uint8_t proto;
	/** Rule's match priority (higher value is higher priority). */
	uint8_t priority;
	/** Unreplied timeout in seconds; 0: use default, -1: never expires. */
	int unreply_timeout;
	/** Replied timeout in seconds; 0: use default, -1: never expires. */
	int reply_timeout;
};

#if defined(CONFIG_NET_IPV4_NAT)

/**
 * @brief Direction indices for a NAT connection tuple.
 *
 * Used for accessing tuple, interface, etc. by direction origin or reply.
 */
enum {
	/** Traffic in the original direction. */
	DIR_ORIGIN,
	/** Reply direction of the connection. */
	DIR_REPLY,
	/** Number of directions; must be last. */
	DIR_NUM
};

/**
 * @brief A tuple uniquely identifying a connection.
 *
 * Stores source/destination IPv4 address/port/protocol for use with NAT.
 */
struct conn_tuple {
	/** Source IPv4 address. */
	uint8_t src[NET_IPV4_ADDR_SIZE];
	/** Destination IPv4 address. */
	uint8_t dst[NET_IPV4_ADDR_SIZE];
	/** Protocol number (e.g., IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP). */
	uint8_t proto;
	union {
		struct {
			/** TCP source port. */
			uint16_t src_port;
			/** TCP destination port. */
			uint16_t dst_port;
		} tcp; /**< Used if proto is IPPROTO_TCP. */

		struct {
			/** UDP source port. */
			uint16_t src_port;
			/** UDP destination port. */
			uint16_t dst_port;
		} udp; /**< Used if proto is IPPROTO_UDP. */

		struct {
			/** ICMP echo identifier. */
			uint16_t identifier;
		} icmp; /**< Used if proto is IPPROTO_ICMP. */
	};
};

/**
 * @brief NAT table entry for a single connection.
 *
 * Manages state, timeout, interface, and connection tuples for both directions.
 */
struct nat4_entry {
	/** Connection tuple for each direction. */
	struct conn_tuple tuple[DIR_NUM];
	/** Connection state (implementation-defined). */
	int state;
	/** Input/output network interfaces. */
	struct net_if *iface[DIR_NUM];
	/** Unreplied connection timeout configuration (seconds). */
	int unreply_timeout;
	/** Replied connection timeout configuration (seconds). */
	int reply_timeout;
	/** Remaining timeout (seconds). */
	int timeout;
	/** Timestamp of last activity (kernel ticks or microseconds). */
	int64_t last_seen;
	/** Next scheduled timeout timestamp. */
	int64_t next_timeout;
};

/**
 * @brief Represents an iptable rule for IPv4 NAT or forwarding.
 *
 * Captures all configuration for rule matching, policy, and timeouts.
 */
struct iptable_rule {
	/** Node for including rule in a singly-linked list. */
	sys_snode_t node;
	/** Input/output network interfaces. */
	struct net_if *iface[DIR_NUM];
	/** Source IPv4 address. */
	uint8_t src[NET_IPV4_ADDR_SIZE];
	/** Source IPv4 address mask. */
	uint8_t src_mask[NET_IPV4_ADDR_SIZE];
	/** Destination IPv4 address. */
	uint8_t dst[NET_IPV4_ADDR_SIZE];
	/** Destination IPv4 address mask. */
	uint8_t dst_mask[NET_IPV4_ADDR_SIZE];
	/** Protocol number to match. */
	uint8_t proto;
	/**
	 * Rule match priority (0-255).
	 * Higher priority rules are matched first; for equal priority,
	 * rules added earlier are preferred.
	 */
	uint8_t priority;
	/** Unreplied timeout in seconds; 0: default, -1: never expires. */
	int unreply_timeout;
	/** Replied timeout in seconds; 0: default, -1: never expires. */
	int reply_timeout;
	/** Index in rule pool. */
	uint8_t idx;
	/** Rule usage flag. */
	uint8_t used;
};

/**
 * @brief NAT and iptable state management structure.
 *
 * Stores NAT connection entries and rule list, and tracks timeout state.
 */
struct ip4_table {
	/** NAT entries. */
	struct nat4_entry table[CONFIG_NET_IPV4_CONN_TRACK_MAX_NUM];
	/** List of iptable rules. */
	sys_slist_t rules;
	/** Time reference for timeouts (kernel ticks or microseconds). */
	int64_t now;
};

/**
 * @brief Find an iptable rule that matches the specified parameters.
 *
 * @param param Rule match/search criteria.
 * @return Pointer to a matching iptable_rule if found, NULL otherwise.
 */
struct iptable_rule *net_ipv4_table_rule_get(struct iptable_rule_params *param);

/**
 * @brief Add a new iptable rule.
 *
 * @param param Rule configuration and match criteria.
 * @return Non-negative rule index on success, negative code on error.
 */
int net_ipv4_table_rule_add(struct iptable_rule_params *param);

/**
 * @brief Remove an iptable rule by index.
 *
 * @param idx Index of the rule to remove.
 */
void net_ipv4_table_rule_del(int idx);

/**
 * @brief Initialize IPv4 NAT subsystem.
 *
 * Prepares internal NAT state for operation.
 */
void net_ipv4_nat_init(void);

#else /* CONFIG_NET_IPV4_NAT not defined */

/**
 * @brief Inline stub for adding an iptable rule (when NAT is disabled).
 *
 * @param param Ignored.
 * @return Always returns -1.
 */
static inline int net_ipv4_table_rule_add(struct iptable_rule_params *param)
{
	return -1;
}

/**
 * @brief Inline stub for deleting an iptable rule (when NAT is disabled).
 * @param idx Ignored.
 */
static inline void net_ipv4_table_rule_del(int idx)
{
	/* Do nothing */
}

/**
 * @brief Initialize IPv4 NAT subsystem (when NAT is disabled).
 */
static inline void net_ipv4_nat_init(void)
{
	/* Do nothing */
}
#endif /* CONFIG_NET_IPV4_NAT */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_IPV4_NAT_H_ */
