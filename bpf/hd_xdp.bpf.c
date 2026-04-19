// SPDX-License-Identifier: GPL-2.0
// XDP program for HD relay STUN/TURN fast path.
//
// Handles STUN Binding Requests at line rate by
// building Binding Responses directly in XDP, and
// forwards TURN ChannelData by rewriting headers
// via BPF map lookup.

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define STUN_MAGIC_COOKIE	0x2112A442
#define STUN_BINDING_REQUEST	0x0001
#define STUN_BINDING_RESPONSE	0x0101
#define STUN_HEADER_SIZE	20
#define STUN_ATTR_XOR_MAPPED	0x0020
#define TURN_CHANNEL_MIN	0x4000
#define TURN_CHANNEL_MAX	0x7FFF

// XOR-MAPPED-ADDRESS attribute for IPv4:
//   2B type + 2B length + 1B reserved + 1B family
//   + 2B xor-port + 4B xor-addr = 12 bytes.
#define XOR_MAPPED_ATTR_SIZE	12

// STUN address family for IPv4.
#define STUN_ADDR_FAMILY_IPV4	0x01

// Stats indices.
#define STAT_STUN_REQUESTS	0
#define STAT_STUN_RESPONSES	1
#define STAT_CHANNEL_FORWARDS	2
#define STAT_PASSED_TO_STACK	3

// -- BPF Maps -----------------------------------------------

struct turn_chan_key {
	__u16 channel;
	__u32 src_ip;
	__u16 src_port;
} __attribute__((packed));

struct turn_chan_val {
	__u32 peer_ip;
	__u16 peer_port;
	__u8  peer_mac[6];
	__u32 relay_ip;
	__u16 relay_port;
} __attribute__((packed));

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 65536);
	__type(key, struct turn_chan_key);
	__type(value, struct turn_chan_val);
} turn_channels SEC(".maps");

// Stats: 0=stun_requests, 1=stun_responses,
//        2=channel_forwards, 3=passed_to_stack.
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 4);
	__type(key, __u32);
	__type(value, __u64);
} xdp_stats SEC(".maps");

// STUN port (configurable via map).
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u16);
} stun_port SEC(".maps");

// -- Helpers ------------------------------------------------

static __always_inline void
inc_stat(__u32 idx)
{
	__u64 *val = bpf_map_lookup_elem(&xdp_stats, &idx);
	if (val)
		__sync_fetch_and_add(val, 1);
}

static __always_inline __u16
ip_checksum(struct iphdr *ip, void *data_end)
{
	__u32 sum = 0;
	__u16 *p = (__u16 *)ip;

	// IP header is 20 bytes = 10 u16 words.
	#pragma unroll
	for (int i = 0; i < 10; i++) {
		if ((void *)(p + 1) > data_end)
			return 0;
		sum += *p++;
	}
	sum = (sum >> 16) + (sum & 0xFFFF);
	sum += sum >> 16;
	return ~sum;
}

static __always_inline void
swap_mac(struct ethhdr *eth)
{
	__u8 tmp[ETH_ALEN];
	__builtin_memcpy(tmp, eth->h_source, ETH_ALEN);
	__builtin_memcpy(eth->h_source, eth->h_dest, ETH_ALEN);
	__builtin_memcpy(eth->h_dest, tmp, ETH_ALEN);
}

// -- STUN handler -------------------------------------------

static __always_inline int
handle_stun(struct xdp_md *ctx, struct ethhdr *eth,
	    struct iphdr *ip, struct udphdr *udp,
	    void *payload, void *data_end)
{
	// Need at least the STUN header.
	if (payload + STUN_HEADER_SIZE > data_end)
		return XDP_PASS;

	// Read message type (first 2 bytes, network order).
	__u16 msg_type = bpf_ntohs(*(__u16 *)payload);
	if (msg_type != STUN_BINDING_REQUEST) {
		inc_stat(STAT_PASSED_TO_STACK);
		return XDP_PASS;
	}

	inc_stat(STAT_STUN_REQUESTS);

	// We need to build a Binding Response with
	// XOR-MAPPED-ADDRESS. Total STUN payload will be
	// header (20B) + XOR-MAPPED-ADDRESS attr (12B) = 32B.
	//
	// Current UDP payload length may differ. We need to
	// adjust the packet. Use bpf_xdp_adjust_tail to set
	// the exact size we need.
	int current_payload_len =
		(int)((long)data_end - (long)payload);
	int desired_payload_len =
		STUN_HEADER_SIZE + XOR_MAPPED_ATTR_SIZE;
	int delta = desired_payload_len - current_payload_len;

	if (delta != 0) {
		int ret = bpf_xdp_adjust_tail(ctx, delta);
		if (ret < 0) {
			inc_stat(STAT_PASSED_TO_STACK);
			return XDP_PASS;
		}
		// Re-derive all pointers after adjust_tail.
		void *data = (void *)(long)ctx->data;
		data_end = (void *)(long)ctx->data_end;

		eth = (struct ethhdr *)data;
		if ((void *)(eth + 1) > data_end)
			return XDP_PASS;
		ip = (struct iphdr *)(eth + 1);
		if ((void *)(ip + 1) > data_end)
			return XDP_PASS;
		udp = (struct udphdr *)(ip + 1);
		if ((void *)(udp + 1) > data_end)
			return XDP_PASS;
		payload = (void *)(udp + 1);
	}

	// Bounds check the response payload.
	if (payload + desired_payload_len > data_end) {
		inc_stat(STAT_PASSED_TO_STACK);
		return XDP_PASS;
	}

	// Save original client address before we swap.
	__u32 client_ip = ip->saddr;
	__u16 client_port = udp->source;

	// Save transaction ID (bytes 8..19 of STUN header).
	// We copy before modifying the payload.
	__u8 txn_id[12];
	if (payload + 20 > data_end)
		return XDP_PASS;
	__builtin_memcpy(txn_id, payload + 8, 12);

	// Write STUN Binding Response header.
	__u8 *p = (__u8 *)payload;

	// Message type: Binding Response (0x0101).
	*(__u16 *)p = bpf_htons(STUN_BINDING_RESPONSE);

	// Message length: body after header = 12 bytes.
	*(__u16 *)(p + 2) =
		bpf_htons(XOR_MAPPED_ATTR_SIZE);

	// Magic cookie.
	*(__u32 *)(p + 4) = bpf_htonl(STUN_MAGIC_COOKIE);

	// Transaction ID (echo back).
	__builtin_memcpy(p + 8, txn_id, 12);

	// Write XOR-MAPPED-ADDRESS attribute.
	__u8 *attr = p + STUN_HEADER_SIZE;

	// Attribute type: XOR-MAPPED-ADDRESS (0x0020).
	*(__u16 *)attr = bpf_htons(STUN_ATTR_XOR_MAPPED);

	// Attribute length: 8 bytes (1B reserved + 1B family
	// + 2B port + 4B addr).
	*(__u16 *)(attr + 2) = bpf_htons(8);

	// Reserved byte.
	attr[4] = 0;

	// Address family: IPv4.
	attr[5] = STUN_ADDR_FAMILY_IPV4;

	// XOR-mapped port: port XOR top 16 bits of cookie.
	__u16 xor_port =
		client_port ^ bpf_htons(0x2112);
	*(__u16 *)(attr + 6) = xor_port;

	// XOR-mapped address: addr XOR cookie.
	__u32 xor_addr =
		client_ip ^ bpf_htonl(STUN_MAGIC_COOKIE);
	*(__u32 *)(attr + 8) = xor_addr;

	// Swap Ethernet addresses.
	swap_mac(eth);

	// Swap IP addresses.
	ip->saddr = ip->daddr;
	ip->daddr = client_ip;

	// Update IP total length.
	__u16 new_ip_len =
		sizeof(struct iphdr) + sizeof(struct udphdr)
		+ desired_payload_len;
	ip->tot_len = bpf_htons(new_ip_len);

	// Recalculate IP TTL.
	ip->ttl = 64;

	// Recalculate IP checksum.
	ip->check = 0;
	ip->check = ip_checksum(ip, data_end);

	// Swap UDP ports.
	udp->source = udp->dest;
	udp->dest = client_port;

	// Update UDP length.
	__u16 new_udp_len =
		sizeof(struct udphdr) + desired_payload_len;
	udp->len = bpf_htons(new_udp_len);

	// Zero UDP checksum (valid for IPv4 per RFC 768).
	udp->check = 0;

	inc_stat(STAT_STUN_RESPONSES);
	return XDP_TX;
}

// -- TURN ChannelData handler -------------------------------

static __always_inline int
handle_channel(struct xdp_md *ctx, struct ethhdr *eth,
	       struct iphdr *ip, struct udphdr *udp,
	       void *payload, void *data_end)
{
	// ChannelData header: 2B channel + 2B data length.
	if (payload + 4 > data_end)
		return XDP_PASS;

	__u16 channel = bpf_ntohs(*(__u16 *)payload);
	__u16 data_len = bpf_ntohs(*(__u16 *)(payload + 2));

	// Verify the entire ChannelData fits.
	// Pad to 4-byte boundary per RFC 5766.
	__u16 padded_len = (data_len + 3) & ~3;
	if (payload + 4 + padded_len > data_end) {
		inc_stat(STAT_PASSED_TO_STACK);
		return XDP_PASS;
	}

	// Look up channel binding.
	struct turn_chan_key key = {
		.channel = channel,
		.src_ip = ip->saddr,
		.src_port = udp->source,
	};

	struct turn_chan_val *val =
		bpf_map_lookup_elem(&turn_channels, &key);
	if (!val) {
		inc_stat(STAT_PASSED_TO_STACK);
		return XDP_PASS;
	}

	// Rewrite destination MAC.
	__builtin_memcpy(eth->h_dest, val->peer_mac,
			 ETH_ALEN);
	// Source MAC stays (our NIC).

	// Rewrite IP dst.
	__u32 old_daddr = ip->daddr;
	ip->daddr = val->peer_ip;
	// Source becomes the relay IP.
	ip->saddr = val->relay_ip;
	ip->ttl = 64;

	// Recalculate IP checksum.
	ip->check = 0;
	ip->check = ip_checksum(ip, data_end);

	// Rewrite UDP dst port.
	udp->dest = val->peer_port;
	// Source becomes the relay port.
	udp->source = val->relay_port;

	// Zero UDP checksum (valid for IPv4).
	udp->check = 0;

	inc_stat(STAT_CHANNEL_FORWARDS);
	return XDP_TX;
}

// -- Main entry point ---------------------------------------

SEC("xdp")
int hd_stun_turn(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;

	// Parse Ethernet.
	struct ethhdr *eth = data;
	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;
	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return XDP_PASS;

	// Parse IPv4.
	struct iphdr *ip = (void *)(eth + 1);
	if ((void *)(ip + 1) > data_end)
		return XDP_PASS;
	if (ip->protocol != IPPROTO_UDP)
		return XDP_PASS;
	// No IP options.
	if (ip->ihl != 5)
		return XDP_PASS;

	// Parse UDP.
	struct udphdr *udp = (void *)(ip + 1);
	if ((void *)(udp + 1) > data_end)
		return XDP_PASS;

	// Check destination port against configured STUN port.
	__u32 key0 = 0;
	__u16 *port = bpf_map_lookup_elem(&stun_port, &key0);
	if (!port || udp->dest != bpf_htons(*port))
		return XDP_PASS;

	void *payload = (void *)(udp + 1);
	int udp_payload_len =
		bpf_ntohs(udp->len) - sizeof(*udp);
	if (udp_payload_len < 4)
		return XDP_PASS;
	if (payload + udp_payload_len > data_end)
		return XDP_PASS;

	// Check for STUN magic cookie at offset 4.
	if (udp_payload_len >= STUN_HEADER_SIZE) {
		__u32 cookie;
		if (payload + 8 > data_end)
			return XDP_PASS;
		cookie = *(__u32 *)(payload + 4);
		if (cookie == bpf_htonl(STUN_MAGIC_COOKIE)) {
			return handle_stun(ctx, eth, ip, udp,
					   payload, data_end);
		}
	}

	// Check for TURN ChannelData (0x4000..0x7FFF).
	if (payload + 2 > data_end)
		return XDP_PASS;
	__u16 first_word = bpf_ntohs(*(__u16 *)payload);
	if (first_word >= TURN_CHANNEL_MIN &&
	    first_word <= TURN_CHANNEL_MAX) {
		return handle_channel(ctx, eth, ip, udp,
				      payload, data_end);
	}

	inc_stat(STAT_PASSED_TO_STACK);
	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
