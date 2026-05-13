/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(net_ipv4, CONFIG_NET_IPV4_LOG_LEVEL);

#include <zephyr/net/ipv4_nat.h>
#include <zephyr/net/net_log.h>
#include <zephyr/net/net_pkt_filter.h>
#include <zephyr/net/icmp.h>
#include "net_private.h"
#include "icmpv4.h"

struct ip4_table iptables;
struct iptable_rule iptable_rules[CONFIG_NET_IPV4_IPTABLE_RULE_MAX_NUM];

#define IP_HWORD(p) (UNALIGNED_GET((uint16_t *)((p) + 2)))
#define IP_LWORD(p) (UNALIGNED_GET((uint16_t *)(p)))
#define IP_DWORD(p) (UNALIGNED_GET((uint32_t *)(p)))

#define IPSTR "%hhu.%hhu.%hhu.%hhu"
#define IP2STR(a) (a)[0], (a)[1], (a)[2], (a)[3]

#define CT_NOW() k_uptime_ticks()
#define CT_TIMEOUT(s) k_sec_to_ticks_ceil32(s)
#define CT_FOREVER() (-1)

#define DEFAULT_UNREPLY_TIMEOUT 5
#define DEFAULT_REPLY_TIMEOUT 60

enum {
	CONN_TRACK_UNUSED,
	CONN_TRACK_WAITING,
	CONN_TRACK_ESTABLISHED
};

static inline uint32_t sum_update(uint16_t old_val, uint16_t new_val)
{
	uint32_t tmp = (~old_val & 0xFFFF) + new_val;

	while (tmp >> 16) {
		tmp = (tmp & 0xFFFF) + (tmp >> 16);
	}

	return tmp;
}

static uint16_t update_chksum(uint16_t old_chksum, uint32_t delta)
{
	uint32_t sum = (~old_chksum & 0xFFFF) + delta;

	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}

	return (uint16_t)(~sum & 0xFFFF);
}

static bool conn_track_match_reply(struct net_pkt *pkt,
				   struct net_ipv4_hdr *iphdr,
				   void *l4hdr,
				   struct conn_tuple *tuple)
{
	if (iphdr->proto != tuple->proto ||
	    !net_ipv4_addr_cmp_raw(iphdr->src, tuple->src) ||
	    !net_ipv4_addr_cmp_raw(iphdr->dst, tuple->dst)) {
		return false;
	}

	if (tuple->proto == NET_IPPROTO_ICMP) {
		struct net_icmp_hdr *icmphdr = (struct net_icmp_hdr *)l4hdr;

		if (icmphdr->type == NET_ICMPV4_ECHO_REPLY &&
		    net_pkt_get_contiguous_len(pkt) >= NET_ICMPV4H_LEN + NET_ICMPV4_UNUSED_LEN &&
		    UNALIGNED_GET((uint16_t *)((uint8_t *)l4hdr +
		    sizeof(struct net_icmp_hdr))) == tuple->icmp.identifier) {
			return true;
		}
	} else if (tuple->proto == NET_IPPROTO_TCP) {
		struct net_tcp_hdr *tcphdr = (struct net_tcp_hdr *)l4hdr;

		if (net_pkt_get_contiguous_len(pkt) >= NET_TCPH_LEN &&
		    tcphdr->src_port == tuple->tcp.src_port &&
		    tcphdr->dst_port == tuple->tcp.dst_port) {
			return true;
		}
	} else if (tuple->proto == NET_IPPROTO_UDP) {
		struct net_udp_hdr *udphdr = (struct net_udp_hdr *)l4hdr;

		if (net_pkt_get_contiguous_len(pkt) >= NET_UDPH_LEN &&
		    udphdr->src_port == tuple->udp.src_port &&
		    udphdr->dst_port == tuple->udp.dst_port) {
			return true;
		}
	}

	return false;
}

static bool conn_track_match_origin(struct net_pkt *pkt,
				    struct net_ipv4_hdr *iphdr,
				    void *l4hdr,
				    struct conn_tuple *tuple)
{
	if (iphdr->proto != tuple->proto ||
	    !net_ipv4_addr_cmp_raw(iphdr->src, tuple->src) ||
	    !net_ipv4_addr_cmp_raw(iphdr->dst, tuple->dst)) {
		return false;
	}

	if (tuple->proto == NET_IPPROTO_ICMP) {
		struct net_icmp_hdr *icmphdr = (struct net_icmp_hdr *)l4hdr;

		if (icmphdr->type == NET_ICMPV4_ECHO_REQUEST &&
		    net_pkt_get_contiguous_len(pkt) >= NET_ICMPV4H_LEN + NET_ICMPV4_UNUSED_LEN) {
			/* update IN tuple because ICMP has only one request in the air */
			tuple->icmp.identifier = *(uint16_t *)((uint8_t *)l4hdr +
				sizeof(struct net_icmp_hdr));
			return true;
		}
	} else if (tuple->proto == NET_IPPROTO_TCP) {
		struct net_tcp_hdr *tcphdr = (struct net_tcp_hdr *)l4hdr;

		if (net_pkt_get_contiguous_len(pkt) >= NET_TCPH_LEN &&
		    tcphdr->src_port == tuple->tcp.src_port &&
		    tcphdr->dst_port == tuple->tcp.dst_port) {
			return true;
		}
	} else if (tuple->proto == NET_IPPROTO_UDP) {
		struct net_udp_hdr *udphdr = (struct net_udp_hdr *)l4hdr;

		if (net_pkt_get_contiguous_len(pkt) >= NET_UDPH_LEN &&
		    udphdr->src_port == tuple->udp.src_port &&
		    udphdr->dst_port == tuple->udp.dst_port) {
			return true;
		}
	}

	return false;
}

static bool conn_track_new(struct net_pkt *pkt,
			   struct net_ipv4_hdr *iphdr,
			   void *l4hdr,
			   struct nat4_entry *entry)
{
	struct conn_tuple *in = &entry->tuple[DIR_ORIGIN];
	struct conn_tuple *out = &entry->tuple[DIR_REPLY];
	struct net_if *reply = entry->iface[DIR_REPLY];
	struct net_if_addr *ifaddr;

	ifaddr = net_if_ipv4_addr_get_first_by_index(net_if_get_by_iface(reply));
	if (!ifaddr) {
		return false;
	}

	if (iphdr->proto == NET_IPPROTO_ICMP) {
		struct net_icmp_hdr *icmphdr = (struct net_icmp_hdr *)l4hdr;

		if (icmphdr->type != NET_ICMPV4_ECHO_REQUEST) {
			return false;
		}

		in->icmp.identifier = *(uint16_t *)(l4hdr + sizeof(struct net_icmp_hdr));
		out->icmp.identifier = *(uint16_t *)(l4hdr + sizeof(struct net_icmp_hdr));
		in->proto = NET_IPPROTO_ICMP;
		out->proto = NET_IPPROTO_ICMP;
	} else if (iphdr->proto == NET_IPPROTO_TCP) {
		struct net_tcp_hdr *tcphdr = (struct net_tcp_hdr *)l4hdr;

		in->tcp.src_port = tcphdr->src_port;
		in->tcp.dst_port = tcphdr->dst_port;
		out->tcp.src_port = tcphdr->dst_port;
		out->tcp.dst_port = tcphdr->src_port;
		in->proto = NET_IPPROTO_TCP;
		out->proto = NET_IPPROTO_TCP;
	} else if (iphdr->proto == NET_IPPROTO_UDP) {
		struct net_udp_hdr *udphdr = (struct net_udp_hdr *)l4hdr;

		in->udp.src_port = udphdr->src_port;
		in->udp.dst_port = udphdr->dst_port;
		out->udp.src_port = udphdr->dst_port;
		out->udp.dst_port = udphdr->src_port;
		in->proto = NET_IPPROTO_UDP;
		out->proto = NET_IPPROTO_UDP;
	} else {
		return false;
	}

	net_ipv4_addr_copy_raw(in->src, iphdr->src);
	net_ipv4_addr_copy_raw(in->dst, iphdr->dst);
	net_ipv4_addr_copy_raw(out->src, iphdr->dst);
	net_ipv4_addr_copy_raw(out->dst, ifaddr->address.in_addr.s4_addr);

	entry->timeout = (entry->unreply_timeout == CT_FOREVER()) ?
			  CT_FOREVER() : CT_TIMEOUT(entry->unreply_timeout);
	entry->state = CONN_TRACK_WAITING;
	return true;
}

static inline void conn_track_free(struct nat4_entry *entry)
{
	memset(entry, 0x0, sizeof(struct nat4_entry));
}

static inline void conn_track_refresh(struct nat4_entry *entry)
{
	if (entry->timeout != CT_FOREVER()) {
		entry->last_seen = CT_NOW();
		entry->next_timeout = entry->last_seen + entry->timeout;
	}
}

static void conn_track_confirm(struct nat4_entry *entry)
{
	if (entry->state == CONN_TRACK_WAITING) {
		entry->state = CONN_TRACK_ESTABLISHED;
		entry->timeout = (entry->reply_timeout == CT_FOREVER()) ?
				  CT_FOREVER() : CT_TIMEOUT(entry->reply_timeout);
	}
	conn_track_refresh(entry);
}

static inline bool conn_track_is_expired(struct nat4_entry *entry)
{
	return (entry->timeout == CT_FOREVER()) ?
		false : (iptables.now >= entry->next_timeout);
}

static int conn_track_match(struct net_pkt *pkt,
			    struct net_ipv4_hdr *iphdr,
			    void *l4hdr,
			    struct nat4_entry **entry)
{
	int i;

	/* ipforward work active */
	iptables.now = CT_NOW();

	for (i = 0; i < CONFIG_NET_IPV4_CONN_TRACK_MAX_NUM; i++) {
		if (!iptables.table[i].state) {
			continue;
		}

		if (conn_track_match_origin(pkt, iphdr,
		    l4hdr, &iptables.table[i].tuple[DIR_ORIGIN])) {
			NET_INFO("find IN conn track %d", i);
			conn_track_refresh(&iptables.table[i]);
			*entry = &iptables.table[i];
			return DIR_ORIGIN;
		}

		if (conn_track_match_reply(pkt, iphdr,
		    l4hdr, &iptables.table[i].tuple[DIR_REPLY])) {
			NET_INFO("find OUT conn track %d", i);
			conn_track_confirm(&iptables.table[i]);
			*entry = &iptables.table[i];
			return DIR_REPLY;
		}

		if (conn_track_is_expired(&iptables.table[i])) {
			NET_INFO("conn track %d expired", i);
			conn_track_free(&iptables.table[i]);
		}
	}

	return DIR_NUM;
}

static struct nat4_entry *conn_track_create(struct net_pkt *pkt,
						 struct net_ipv4_hdr *iphdr,
						 void *l4hdr,
						 struct iptable_rule *rule)
{
	int i;
	struct nat4_entry *find;

	for (i = 0; i < CONFIG_NET_IPV4_CONN_TRACK_MAX_NUM; i++) {
		if (iptables.table[i].state == CONN_TRACK_UNUSED) {
			find = &iptables.table[i];
			goto new_entry;
		}
	}

	return NULL;

new_entry:
	find->iface[DIR_ORIGIN] = rule->iface[DIR_ORIGIN];
	find->iface[DIR_REPLY] = rule->iface[DIR_REPLY];
	find->unreply_timeout = rule->unreply_timeout ?
				rule->unreply_timeout : DEFAULT_UNREPLY_TIMEOUT;
	find->reply_timeout = rule->reply_timeout ?
			      rule->reply_timeout : DEFAULT_REPLY_TIMEOUT;
	if (!conn_track_new(pkt, iphdr, l4hdr, find)) {
		return NULL;
	}
	conn_track_refresh(find);
	return find;
}

static void conn_track_scan_expire(void)
{
	int i;

	iptables.now = CT_NOW();

	for (i = 0; i < CONFIG_NET_IPV4_CONN_TRACK_MAX_NUM; i++) {
		if (conn_track_is_expired(&iptables.table[i])) {
			NET_INFO("conn track %d expired", i);
			conn_track_free(&iptables.table[i]);
		}
	}
}

/* change pkt src from original tuple src addr to local iface addr */
static void do_snat(struct net_pkt *pkt,
		    struct net_ipv4_hdr *iphdr,
		    void *l4hdr,
		    struct nat4_entry *entry)
{
	struct conn_tuple *tuple = &entry->tuple[DIR_REPLY];
	uint32_t sum;

	if (iphdr->ttl <= 1) {
		NET_ERR("SNAT pkt hop limit drop");
		return;
	}

	/* update L4 header */
	if (iphdr->proto == NET_IPPROTO_TCP) {
		struct net_tcp_hdr *tcphdr = (struct net_tcp_hdr *)l4hdr;

		sum = sum_update(tcphdr->src_port, tuple->tcp.dst_port);
		sum += sum_update(IP_LWORD(iphdr->src), IP_LWORD(tuple->dst));
		sum += sum_update(IP_HWORD(iphdr->src), IP_HWORD(tuple->dst));

		tcphdr->src_port = tuple->tcp.dst_port;
		tcphdr->chksum = update_chksum(tcphdr->chksum, sum);
	} else if (iphdr->proto == NET_IPPROTO_UDP) {
		struct net_udp_hdr *udphdr = (struct net_udp_hdr *)l4hdr;

		sum = sum_update(udphdr->src_port, tuple->udp.dst_port);
		sum += sum_update(IP_LWORD(iphdr->src), IP_LWORD(tuple->dst));
		sum += sum_update(IP_HWORD(iphdr->src), IP_HWORD(tuple->dst));

		udphdr->src_port = tuple->udp.dst_port;
		udphdr->chksum = update_chksum(udphdr->chksum, sum);
	}

	/* update IP header */
	sum = sum_update(iphdr->ttl, iphdr->ttl - 1);
	sum += sum_update(IP_LWORD(iphdr->src), IP_LWORD(tuple->dst));
	sum += sum_update(IP_HWORD(iphdr->src), IP_HWORD(tuple->dst));
	iphdr->chksum = update_chksum(iphdr->chksum, sum);

	iphdr->ttl--;
	net_ipv4_addr_copy_raw(iphdr->src, tuple->dst);

	/* update target iface */
	net_pkt_set_iface(pkt, entry->iface[DIR_REPLY]);
}

/* change pkt dst from local iface addr to original tuple src addr */
static void do_dnat(struct net_pkt *pkt,
		    struct net_ipv4_hdr *iphdr,
		    void *l4hdr,
		    struct nat4_entry *entry)
{
	struct conn_tuple *tuple = &entry->tuple[DIR_ORIGIN];
	uint32_t sum;

	if (iphdr->ttl <= 1) {
		NET_ERR("DNAT pkt hop limit drop");
		return;
	}

	/* update L4 header */
	if (iphdr->proto == NET_IPPROTO_TCP) {
		struct net_tcp_hdr *tcphdr = (struct net_tcp_hdr *)l4hdr;

		sum = sum_update(tcphdr->dst_port, tuple->tcp.src_port);
		sum += sum_update(IP_LWORD(iphdr->dst), IP_LWORD(tuple->src));
		sum += sum_update(IP_HWORD(iphdr->dst), IP_HWORD(tuple->src));

		tcphdr->dst_port = tuple->tcp.src_port;
		tcphdr->chksum = update_chksum(tcphdr->chksum, sum);
	} else if (iphdr->proto == NET_IPPROTO_UDP) {
		struct net_udp_hdr *udphdr = (struct net_udp_hdr *)l4hdr;

		sum = sum_update(udphdr->dst_port, tuple->udp.src_port);
		sum += sum_update(IP_LWORD(iphdr->dst), IP_LWORD(tuple->src));
		sum += sum_update(IP_HWORD(iphdr->dst), IP_HWORD(tuple->src));

		udphdr->dst_port = tuple->udp.src_port;
		udphdr->chksum = update_chksum(udphdr->chksum, sum);
	}

	/* update IP header */
	sum = sum_update(iphdr->ttl, iphdr->ttl - 1);
	sum += sum_update(IP_LWORD(iphdr->dst), IP_LWORD(tuple->src));
	sum += sum_update(IP_HWORD(iphdr->dst), IP_HWORD(tuple->src));
	iphdr->chksum = update_chksum(iphdr->chksum, sum);

	iphdr->ttl--;
	net_ipv4_addr_copy_raw(iphdr->dst, tuple->src);

	/* update target iface */
	net_pkt_set_iface(pkt, entry->iface[DIR_ORIGIN]);
}

/* iptable rule management */
static void iptable_rule_list_insert(struct iptable_rule *rule)
{
	sys_slist_t *list = &iptables.rules;
	struct iptable_rule *curr = (struct iptable_rule *)sys_slist_peek_head(list);
	struct iptable_rule *next;

	if (curr && curr->priority < rule->priority) {
		sys_slist_prepend(list, &rule->node);
		return;
	}

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(list, curr, next, node) {
		if (next && next->priority < rule->priority) {
			sys_slist_insert(list, &curr->node, &rule->node);
			return;
		}
	}

	sys_slist_append(list, &rule->node);
}

static void iptable_rule_list_remove(struct iptable_rule *rule)
{
	(void)sys_slist_find_and_remove(&iptables.rules, &rule->node);
}

static struct iptable_rule *iptable_rule_alloc(void)
{
	int i;

	for (i = 0; i < CONFIG_NET_IPV4_IPTABLE_RULE_MAX_NUM; i++) {
		if (!iptable_rules[i].used) {
			iptable_rules[i].used = 1;
			iptable_rules[i].idx = i;
			return &iptable_rules[i];
		}
	}

	NET_ERR("iptable rule alloc fail pool empty");
	return NULL;
}

static void iptable_rule_free(struct iptable_rule *rule)
{
	if (rule) {
		if (rule->used) {
			iptable_rule_list_remove(rule);
			rule->used = 0;
		} else {
			NET_ERR("iptable rule double-free %d", rule->idx);
			/* still remove from list in case */
			iptable_rule_list_remove(rule);
		}
	}
}

struct iptable_rule *net_ipv4_table_rule_get(struct iptable_rule_params *param)
{
	struct iptable_rule *rule, *next;
	struct net_if *input_iface, *output_iface;

	if (!param) {
		return NULL;
	}

	input_iface = net_if_get_by_index(param->input_iface_idx);
	output_iface = net_if_get_by_index(param->output_iface_idx);
	if (!input_iface || !output_iface) {
		return NULL;
	}

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&iptables.rules, rule, next, node) {
		if (rule->iface[DIR_ORIGIN] == input_iface &&
		    rule->iface[DIR_REPLY] == output_iface &&
		    rule->proto == param->proto &&
		    IP_DWORD(rule->src) == IP_DWORD(param->src) &&
		    IP_DWORD(rule->src_mask) == IP_DWORD(param->src_mask) &&
		    IP_DWORD(rule->dst) == IP_DWORD(param->dst) &&
		    IP_DWORD(rule->dst_mask) == IP_DWORD(param->dst_mask)) {
			return rule;
		}
	}

	return NULL;
}

int net_ipv4_table_rule_add(struct iptable_rule_params *param)
{
	struct iptable_rule *rule;
	struct net_if *input_iface, *output_iface;

	if (!param) {
		return -EINVAL;
	}

	if (param->proto != NET_IPPROTO_TCP &&
	    param->proto != NET_IPPROTO_UDP &&
	    param->proto != NET_IPPROTO_ICMP) {
		return -EINVAL;
	}

	input_iface = net_if_get_by_index(param->input_iface_idx);
	output_iface = net_if_get_by_index(param->output_iface_idx);
	if (!input_iface || !output_iface) {
		return -EINVAL;
	}

	rule = iptable_rule_alloc();
	if (!rule) {
		return -ENOMEM;
	}

	rule->iface[DIR_ORIGIN] = input_iface;
	rule->iface[DIR_REPLY] = output_iface;
	rule->proto = param->proto;
	memcpy(rule->src, param->src, NET_IPV4_ADDR_SIZE);
	memcpy(rule->src_mask, param->src_mask, NET_IPV4_ADDR_SIZE);
	memcpy(rule->dst, param->dst, NET_IPV4_ADDR_SIZE);
	memcpy(rule->dst_mask, param->dst_mask, NET_IPV4_ADDR_SIZE);

	rule->priority = param->priority;
	rule->unreply_timeout = param->unreply_timeout;
	rule->reply_timeout = param->reply_timeout;

	iptable_rule_list_insert(rule);
	return 0;
}

void net_ipv4_table_rule_del(int idx)
{
	struct iptable_rule *rule;

	if (idx < 0 || idx >= CONFIG_NET_IPV4_IPTABLE_RULE_MAX_NUM) {
		NET_ERR("iptable rule free invalid idx %d", idx);
		return;
	}

	rule = &iptable_rules[idx];
	if (rule->used) {
		iptable_rule_free(rule);
	} else {
		NET_ERR("iptable rule double-free %d", idx);
		/* still remove from list if somehow be there */
		iptable_rule_list_remove(rule);
	}
}

static struct iptable_rule *iptable_rule_match(struct net_pkt *pkt,
					       struct net_ipv4_hdr *iphdr)
{
	struct iptable_rule *rule, *next;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&iptables.rules, rule, next, node) {
		if (net_pkt_iface(pkt) == rule->iface[DIR_ORIGIN] &&
		    iphdr->proto == rule->proto &&
		    (IP_DWORD(iphdr->src) & IP_DWORD(rule->src_mask)) ==
		     (IP_DWORD(rule->src) & IP_DWORD(rule->src_mask)) &&
		    (IP_DWORD(iphdr->dst) & IP_DWORD(rule->dst_mask)) ==
		     (IP_DWORD(rule->dst) & IP_DWORD(rule->dst_mask))) {
			return rule;
		}
	}
	return NULL;
}

static inline bool nat_common_filter(uint8_t proto)
{
	return (proto == NET_IPPROTO_TCP ||
		proto == NET_IPPROTO_UDP ||
		proto == NET_IPPROTO_ICMP);
}

static bool ipv4_nat_process(struct npf_test *test, struct net_pkt *pkt)
{
	NET_PKT_DATA_ACCESS_CONTIGUOUS_DEFINE(ipv4_access, struct net_ipv4_hdr);
	struct net_ipv4_hdr *iphdr;
	struct net_pkt_cursor backup;
	void *l4hdr;
	int dir;
	struct nat4_entry *entry = NULL;
	struct iptable_rule *rule = NULL;
	int ret;

	net_pkt_cursor_backup(pkt, &backup);

	iphdr = (struct net_ipv4_hdr *)net_pkt_get_data(pkt, &ipv4_access);
	if (!iphdr) {
		goto no_match;
	}

	if (net_ipv4_is_addr_mcast_raw(iphdr->dst) ||
	    net_ipv4_is_addr_bcast_raw(net_pkt_iface(pkt), iphdr->dst)) {
		goto no_match;
	}

	if (!nat_common_filter(iphdr->proto)) {
		goto no_match;
	}

	if (IS_ENABLED(CONFIG_NET_IPV4_FRAGMENT)) {
		/* Check if this is a fragmented packet, and if so,
		 * ignore and process it when it is reassembled
		 */
		if ((net_ntohs(*((uint16_t *)&iphdr->offset[0])) &
		     (NET_IPV4_FRAGH_OFFSET_MASK | NET_IPV4_MORE_FRAG_MASK)) != 0) {
			goto no_match;
		}
	}

	ret = net_pkt_acknowledge_data(pkt, &ipv4_access);
	if (ret < 0) {
		goto no_match;
	}

	if (net_pkt_ip_opts_len(pkt)) {
		if (net_pkt_skip(pkt, net_pkt_ip_opts_len(pkt))) {
			goto no_match;
		}
	}

	l4hdr = net_pkt_cursor_get_pos(pkt);
	if (!l4hdr) {
		goto no_match;
	}

	dir = conn_track_match(pkt, iphdr, l4hdr, &entry);
	if (dir == DIR_ORIGIN) {
		do_snat(pkt, iphdr, l4hdr, entry);
		goto match;
	} else if (dir == DIR_REPLY) {
		do_dnat(pkt, iphdr, l4hdr, entry);
		goto match;
	}

	rule = iptable_rule_match(pkt, iphdr);
	if (!rule) {
		goto no_match;
	}

	/* new conntrack */
	entry = conn_track_create(pkt, iphdr, l4hdr, rule);
	if (!entry) {
		conn_track_scan_expire();
		entry = conn_track_create(pkt, iphdr, l4hdr, rule);
		if (!entry) {
			goto no_match;
		}
	}

	do_snat(pkt, iphdr, l4hdr, entry);
match:
	net_pkt_lladdr_clear(pkt);
	net_pkt_cursor_restore(pkt, &backup);
	return true;
no_match:
	net_pkt_cursor_restore(pkt, &backup);
	return false;
}

struct iptable4_pkt_filter_test {
	struct npf_test test;
};

static struct iptable4_pkt_filter_test ipv4_iptable_rule_check = {
	.test.fn = ipv4_nat_process
};

static NPF_RULE(ipv4_nat_pkt_process, NET_OK, ipv4_iptable_rule_check);

void net_ipv4_nat_init(void)
{
	sys_slist_init(&iptables.rules);
	npf_insert_ipv4_recv_rule(&ipv4_nat_pkt_process);
	npf_append_ipv4_recv_rule(&npf_default_ok);
}
