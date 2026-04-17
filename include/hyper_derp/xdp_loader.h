/// @file xdp_loader.h
/// @brief XDP program loader for STUN/TURN fast path.
///   Loads and attaches the BPF XDP program, manages
///   TURN channel map entries, and reads per-CPU stats.

#ifndef INCLUDE_HYPER_DERP_XDP_LOADER_H_
#define INCLUDE_HYPER_DERP_XDP_LOADER_H_

#include <cstdint>

namespace hyper_derp {

/// Configuration for XDP attachment.
struct XdpConfig {
  /// Network interface name (e.g. "eth0").
  const char* interface = nullptr;
  /// STUN/TURN listen port.
  uint16_t stun_port = 3478;
  /// Path to the compiled BPF object file.
  const char* bpf_obj_path = "bpf/hd_xdp.bpf.o";
  /// Enable XDP attachment.
  bool enabled = false;
};

/// Opaque handle for an attached XDP program.
struct XdpContext {
  int prog_fd = -1;
  int ifindex = 0;
  int channels_map_fd = -1;
  int stats_map_fd = -1;
  int port_map_fd = -1;
  void* bpf_obj = nullptr;
  bool attached = false;
};

/// Load and attach the XDP program to the interface.
/// @param ctx Output context (zeroed by caller).
/// @param config Interface name, port, BPF object path.
/// @return 0 on success, negative errno on failure.
int XdpLoad(XdpContext* ctx, const XdpConfig* config);

/// Detach the XDP program and close all fds.
void XdpUnload(XdpContext* ctx);

/// Insert or update a TURN channel binding in the BPF map.
/// @param ctx Loaded XDP context.
/// @param channel TURN channel number (0x4000..0x7FFF).
/// @param src_ip Client source IP (network byte order).
/// @param src_port Client source port (network byte order).
/// @param peer_ip Peer destination IP (network byte order).
/// @param peer_port Peer destination port (network order).
/// @param peer_mac Peer destination MAC (6 bytes).
/// @param relay_ip Relay source IP (network byte order).
/// @param relay_port Relay source port (network byte order).
/// @return 0 on success, negative errno on failure.
int XdpUpdateChannel(XdpContext* ctx,
                     uint16_t channel,
                     uint32_t src_ip,
                     uint16_t src_port,
                     uint32_t peer_ip,
                     uint16_t peer_port,
                     const uint8_t* peer_mac,
                     uint32_t relay_ip,
                     uint16_t relay_port);

/// Remove a TURN channel binding from the BPF map.
/// @param ctx Loaded XDP context.
/// @param channel TURN channel number.
/// @param src_ip Client source IP (network byte order).
/// @param src_port Client source port (network byte order).
/// @return 0 on success, negative errno on failure.
int XdpRemoveChannel(XdpContext* ctx,
                     uint16_t channel,
                     uint32_t src_ip,
                     uint16_t src_port);

/// Aggregated XDP statistics.
struct XdpStats {
  uint64_t stun_requests = 0;
  uint64_t stun_responses = 0;
  uint64_t channel_forwards = 0;
  uint64_t passed_to_stack = 0;
};

/// Read XDP stats counters, summed across all CPUs.
/// @param ctx Loaded XDP context.
/// @param stats Output stats struct.
/// @return 0 on success, negative errno on failure.
int XdpGetStats(XdpContext* ctx, XdpStats* stats);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_XDP_LOADER_H_
