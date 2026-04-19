// SPDX-License-Identifier: GPL-2.0
// Minimal XDP program for AF_XDP socket redirect.
//
// Redirects UDP packets matching the configured port
// to the AF_XDP socket bound on the receiving queue.
// All other traffic passes through to the kernel stack.

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

// XSK map: queue_id -> AF_XDP socket fd.
struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, 64);
	__type(key, __u32);
	__type(value, __u32);
} xsk_map SEC(".maps");

// UDP destination port to intercept.
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u16);
} udp_port SEC(".maps");

// Stats: 0=redirected, 1=passed.
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 2);
	__type(key, __u32);
	__type(value, __u64);
} af_xdp_stats SEC(".maps");

static __always_inline void
inc_stat(__u32 idx)
{
	__u64 *val = bpf_map_lookup_elem(&af_xdp_stats, &idx);
	if (val)
		__sync_fetch_and_add(val, 1);
}

SEC("xdp")
int xdp_af_xdp_redirect(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;

	// Parse Ethernet header.
	struct ethhdr *eth = data;
	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;
	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return XDP_PASS;

	// Parse IPv4 header (no options).
	struct iphdr *ip = (void *)(eth + 1);
	if ((void *)(ip + 1) > data_end)
		return XDP_PASS;
	if (ip->protocol != IPPROTO_UDP)
		return XDP_PASS;
	if (ip->ihl != 5)
		return XDP_PASS;

	// Parse UDP header.
	struct udphdr *udp = (void *)(ip + 1);
	if ((void *)(udp + 1) > data_end)
		return XDP_PASS;

	// Check destination port.
	__u32 key = 0;
	__u16 *port = bpf_map_lookup_elem(&udp_port, &key);
	if (!port || udp->dest != bpf_htons(*port)) {
		inc_stat(1);
		return XDP_PASS;
	}

	// Redirect to AF_XDP socket on this queue.
	int idx = ctx->rx_queue_index;
	inc_stat(0);
	return bpf_redirect_map(&xsk_map, idx, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
