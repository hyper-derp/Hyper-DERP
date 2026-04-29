// SPDX-License-Identifier: GPL-2.0
// XDP fast path for `mode: wireguard`.
//
// Looks up the source 4-tuple of an inbound UDP packet in
// the operator-managed peer map, finds that peer's link
// partner, rewrites L2/L3/L4 headers, and bounces the
// packet back out the same NIC via XDP_TX. Anything we
// can't handle (unknown source, partner MAC not yet
// learned, non-IPv4, IP options present) returns XDP_PASS
// and falls through to the userspace forwarder, which
// keeps `mode: wireguard` correct on every kernel /
// driver combo even when the fast path can't fire.
//
// Maps:
//   wg_peers     : endpoint -> partner endpoint
//   wg_macs      : endpoint -> Ethernet src MAC observed
//                  on the last packet from that endpoint
//   wg_xdp_stats : per-CPU counters
//   wg_port      : array[1] holding the relay's UDP port
//                  in host byte order

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

// Stats indices. Keep in sync with WgXdpStats in
// include/hyper_derp/wg_relay.h.
#define STAT_RX				0
#define STAT_FWD			1
#define STAT_PASS_NO_PEER		2
#define STAT_PASS_NO_MAC		3
#define STAT_DROP_NOT_WG_SHAPED		4
#define STAT_DROP_BLOCKLISTED		5

// -- Map types --------------------------------------------

// Endpoint = (src_ip, src_port). Padding so the verifier
// stays happy and userspace + BPF agree on the layout.
struct ep_key {
	__u32 ip;	// network byte order
	__u16 port;	// network byte order
	__u16 _pad;	// must be zeroed by writers
};

struct ep_val {
	__u32 ip;	// network byte order
	__u16 port;	// network byte order
	__u16 _pad;
	// Egress NIC ifindex for traffic to this partner. When
	// equal to ctx->ingress_ifindex we use XDP_TX; otherwise
	// XDP_REDIRECT through wg_devmap. Zero falls back to
	// XDP_TX for backwards compatibility with single-NIC
	// configs that don't populate this field.
	__u32 ifindex;
};

struct mac_val {
	__u8  mac[6];
	__u16 _pad;
};

// Per-source byte counters. Per-CPU so the hot path is
// contention-free; userspace sums across CPUs in
// `wg peer list`.
struct peer_bytes_val {
	__u64 rx_bytes;
	__u64 fwd_bytes;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, struct ep_key);
	__type(value, struct ep_val);
} wg_peers SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, struct ep_key);
	__type(value, struct mac_val);
} wg_macs SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_HASH);
	__uint(max_entries, 1024);
	__type(key, struct ep_key);
	__type(value, struct peer_bytes_val);
} wg_peer_bytes SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 8);
	__type(key, __u32);
	__type(value, __u64);
} wg_xdp_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u16);
} wg_port SEC(".maps");

// Devmap for cross-NIC XDP_REDIRECT. Userspace populates
// it at attach time with one entry per NIC the relay is
// attached to (key = ifindex, value = ifindex). The
// program calls bpf_redirect_map(&wg_devmap, partner_
// ifindex, 0) when the partner lives on a different NIC.
struct {
	__uint(type, BPF_MAP_TYPE_DEVMAP);
	__uint(max_entries, 64);
	__type(key, __u32);
	__type(value, __u32);
} wg_devmap SEC(".maps");

// Per-NIC source MAC. On a cross-NIC redirect we need
// the egress NIC's MAC as the new Ethernet source — the
// ingress NIC's MAC (which is what we'd otherwise reuse)
// would be wrong on the wire. Userspace populates one
// entry per attached NIC at startup.
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 64);
	__type(key, __u32);	// ifindex
	__type(value, struct mac_val);
} wg_nic_macs SEC(".maps");

// Per-NIC source IPv4. Same reason as wg_nic_macs but
// at L3: WireGuard peers reject packets whose source
// 4-tuple doesn't match the configured peer endpoint, so
// after a cross-NIC redirect the IP source has to match
// the egress NIC, not the ingress NIC. Userspace
// populates one entry per attached NIC.
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 64);
	__type(key, __u32);	// ifindex
	__type(value, __u32);	// ipv4 in network byte order
} wg_nic_ips SEC(".maps");

// Blocklist for source IPs that produced repeated failed
// candidate confirmations (i.e. forged handshakes — they had
// the partner's pubkey but couldn't progress to transport
// data because they don't have the static private key).
// Userspace populates expiry_ns from CLOCK_MONOTONIC; the
// BPF program compares against bpf_ktime_get_ns().  An
// entry whose expiry has passed is treated as not present —
// userspace sweeps the map periodically but a stale-but-
// expired entry is harmless either way.
struct blocklist_entry {
	__u64 expiry_ns;	// monotonic ns; 0 = no longer active
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 8192);
	__type(key, __u32);	// IPv4 src in network byte order
	__type(value, struct blocklist_entry);
} wg_blocklist SEC(".maps");

// -- Helpers ----------------------------------------------

static __always_inline void
inc_stat(__u32 idx)
{
	__u64 *val = bpf_map_lookup_elem(&wg_xdp_stats, &idx);
	if (val)
		__sync_fetch_and_add(val, 1);
}

// Recompute the IPv4 header checksum from scratch. The
// caller has already bounds-checked the header (ihl==5,
// (ip + 1) <= data_end), so this needs no loop and no
// per-word check — the verifier copes with a flat 10-word
// sum where it chokes on the unrolled-loop variant.
static __always_inline __u16
ipv4_csum(struct iphdr *ip)
{
	__u16 *p = (__u16 *)ip;
	__u32 sum = (__u32)p[0] + p[1] + p[2] + p[3] + p[4]
		  + p[5] + p[6] + p[7] + p[8] + p[9];
	sum = (sum >> 16) + (sum & 0xFFFF);
	sum += sum >> 16;
	return ~sum;
}

// -- Entry point ------------------------------------------

SEC("xdp")
int wg_relay_xdp(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;

	// Eth header.
	struct ethhdr *eth = data;
	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;
	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return XDP_PASS;

	// IPv4 header. No options.
	struct iphdr *ip = (void *)(eth + 1);
	if ((void *)(ip + 1) > data_end)
		return XDP_PASS;
	if (ip->protocol != IPPROTO_UDP)
		return XDP_PASS;
	if (ip->ihl != 5)
		return XDP_PASS;

	// UDP header.
	struct udphdr *udp = (void *)(ip + 1);
	if ((void *)(udp + 1) > data_end)
		return XDP_PASS;

	// Gate on the relay's listen port.
	__u32 zero = 0;
	__u16 *cfg_port = bpf_map_lookup_elem(&wg_port, &zero);
	if (!cfg_port)
		return XDP_PASS;
	if (udp->dest != bpf_htons(*cfg_port))
		return XDP_PASS;

	inc_stat(STAT_RX);

	// Dynamic blocklist — drop sources that produced
	// repeated failed candidate confirmations.  Stale
	// entries (expiry in the past) fall through.
	{
		__u32 src_ip = ip->saddr;
		struct blocklist_entry *bl =
			bpf_map_lookup_elem(&wg_blocklist, &src_ip);
		if (bl && bl->expiry_ns > bpf_ktime_get_ns()) {
			inc_stat(STAT_DROP_BLOCKLISTED);
			return XDP_DROP;
		}
	}

	// WG-shape filter — peek at the first byte of the UDP
	// payload and verify it's a WireGuard message type
	// (1 init, 2 response, 3 cookie, 4 transport).  Anything
	// else is either malformed or a non-WG client that ended
	// up at the relay's port; dropping at XDP keeps it off
	// the forward path entirely so the partner never has to
	// process it.  Length sanity covers the fixed-size types;
	// transport-data has variable length capped by MTU.
	__u8 *wg = (void *)(udp + 1);
	if ((void *)(wg + 1) > data_end) {
		inc_stat(STAT_DROP_NOT_WG_SHAPED);
		return XDP_DROP;
	}
	__u16 udp_payload_len =
		bpf_ntohs(udp->len) - sizeof(struct udphdr);
	__u8 wg_type = wg[0];
	if (wg_type == 1) {
		if (udp_payload_len != 148) {
			inc_stat(STAT_DROP_NOT_WG_SHAPED);
			return XDP_DROP;
		}
	} else if (wg_type == 2) {
		if (udp_payload_len != 92) {
			inc_stat(STAT_DROP_NOT_WG_SHAPED);
			return XDP_DROP;
		}
	} else if (wg_type == 3) {
		if (udp_payload_len != 64) {
			inc_stat(STAT_DROP_NOT_WG_SHAPED);
			return XDP_DROP;
		}
	} else if (wg_type == 4) {
		// Transport data: header (16 B) + counter (8 B) +
		// at least the AEAD tag (16 B).
		if (udp_payload_len < 32) {
			inc_stat(STAT_DROP_NOT_WG_SHAPED);
			return XDP_DROP;
		}
	} else {
		inc_stat(STAT_DROP_NOT_WG_SHAPED);
		return XDP_DROP;
	}

	// Hand handshake init (1) and response (2) packets up
	// to userspace: it owns the MAC1 verification + the
	// candidate-then-confirm roaming flow, neither of which
	// fits the XDP verifier comfortably and both of which
	// are rare enough (one handshake per session per ~25 s)
	// that the userspace round trip is free.  Cookie reply
	// (3) and transport data (4) keep the XDP fast path.
	if (wg_type == 1 || wg_type == 2) {
		return XDP_PASS;
	}

	// Look up the source endpoint in the peer map. Miss
	// means either an unregistered peer or one whose
	// source IP/port doesn't match the operator's pin —
	// either way userspace counts it as drop_unknown_src.
	struct ep_key src_key = {
		.ip = ip->saddr,
		.port = udp->source,
		._pad = 0,
	};
	struct ep_val *partner =
		bpf_map_lookup_elem(&wg_peers, &src_key);
	if (!partner) {
		inc_stat(STAT_PASS_NO_PEER);
		return XDP_PASS;
	}

	// Learn the source MAC for this endpoint. Idempotent —
	// rewriting the same MAC every packet is fine; if the
	// peer's L2 changes (it won't on a stable bridge but
	// could on some ARP refresh), we pick up the new one.
	struct mac_val src_mv;
	__builtin_memcpy(src_mv.mac, eth->h_source, 6);
	src_mv._pad = 0;
	bpf_map_update_elem(&wg_macs, &src_key, &src_mv,
			    BPF_ANY);

	// Look up the partner's MAC. Miss means we haven't
	// observed a packet from the partner yet; let the
	// userspace fast path forward it (which writes a real
	// sendto and gets the kernel's ARP/route to fill in
	// the L2). Once both peers have sent at least one
	// packet, every subsequent packet rides XDP_TX.
	struct ep_key dst_key = {
		.ip = partner->ip,
		.port = partner->port,
		._pad = 0,
	};
	struct mac_val *dst_mv =
		bpf_map_lookup_elem(&wg_macs, &dst_key);
	if (!dst_mv) {
		inc_stat(STAT_PASS_NO_MAC);
		return XDP_PASS;
	}

	// L2 rewrite. Same-NIC bounce: source becomes the
	// inbound dest (our NIC's MAC). Cross-NIC redirect:
	// source must be the *egress* NIC's MAC, looked up in
	// wg_nic_macs by the partner's ifindex; otherwise the
	// frame goes out the second NIC with the first NIC's
	// MAC and the L2 segment's switch / peer rejects it.
	__u32 dst_if_l2 = partner->ifindex;
	if (dst_if_l2 == 0 ||
	    dst_if_l2 == ctx->ingress_ifindex) {
		__builtin_memcpy(eth->h_source, eth->h_dest, 6);
	} else {
		struct mac_val *egress_mac =
			bpf_map_lookup_elem(&wg_nic_macs, &dst_if_l2);
		if (!egress_mac) {
			// Operator forgot to populate wg_nic_macs
			// for this ifindex — fall back to userspace.
			inc_stat(STAT_PASS_NO_MAC);
			return XDP_PASS;
		}
		__builtin_memcpy(eth->h_source, egress_mac->mac, 6);
	}
	__builtin_memcpy(eth->h_dest, dst_mv->mac, 6);

	// L3 rewrite. Same-NIC: source is the inbound dest
	// (the relay's ingress IP). Cross-NIC: source must be
	// the *egress* NIC's IP, otherwise the receiving WG
	// peer's endpoint check rejects the packet silently.
	if (dst_if_l2 == 0 ||
	    dst_if_l2 == ctx->ingress_ifindex) {
		ip->saddr = ip->daddr;
	} else {
		__u32 *egress_ip =
			bpf_map_lookup_elem(&wg_nic_ips, &dst_if_l2);
		if (!egress_ip) {
			inc_stat(STAT_PASS_NO_MAC);
			return XDP_PASS;
		}
		ip->saddr = *egress_ip;
	}
	ip->daddr = partner->ip;
	ip->ttl = 64;
	ip->check = 0;
	ip->check = ipv4_csum(ip);

	// L4 rewrite: source becomes the relay's port,
	// destination becomes the partner's port. UDP
	// checksum=0 is valid for IPv4 (RFC 768); avoids
	// the pseudo-header recompute on the hot path.
	udp->source = udp->dest;
	udp->dest = partner->port;
	udp->check = 0;

	// Per-source byte accounting. Use IP total length plus
	// the 14-byte Ethernet header rather than
	// (data_end - data) — the verifier rejects pkt - pkt
	// subtraction even through long casts. tot_len is in
	// network byte order. Idempotent lazy-create on the
	// first packet from this source.
	__u64 plen = (__u64)bpf_ntohs(ip->tot_len) +
		     sizeof(struct ethhdr);
	struct peer_bytes_val *pb =
		bpf_map_lookup_elem(&wg_peer_bytes, &src_key);
	if (pb) {
		pb->rx_bytes  += plen;
		pb->fwd_bytes += plen;
	} else {
		struct peer_bytes_val init = {
			.rx_bytes  = plen,
			.fwd_bytes = plen,
		};
		bpf_map_update_elem(&wg_peer_bytes, &src_key,
				    &init, BPF_ANY);
	}

	inc_stat(STAT_FWD);

	// Same-NIC partner → bounce out the ingress interface.
	// Different-NIC partner → cross-NIC redirect through
	// the devmap. ifindex == 0 means "operator did not
	// populate this field" (older single-NIC configs); fall
	// back to XDP_TX so we stay backward compatible.
	__u32 dst_if = partner->ifindex;
	if (dst_if == 0 || dst_if == ctx->ingress_ifindex) {
		return XDP_TX;
	}
	return bpf_redirect_map(&wg_devmap, dst_if, 0);
}

char _license[] SEC("license") = "GPL";
