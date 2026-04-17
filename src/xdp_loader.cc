/// @file xdp_loader.cc
/// @brief XDP program loader: opens BPF object, attaches
///   to NIC, manages TURN channel map and stats.

#include "hyper_derp/xdp_loader.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <memory>

#include <net/if.h>
#include <linux/if_link.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

namespace hyper_derp {

// Map key/value structs must match the BPF program layout
// exactly (packed).
#pragma pack(push, 1)
struct TurnChanKey {
  uint16_t channel;
  uint32_t src_ip;
  uint16_t src_port;
};
struct TurnChanVal {
  uint32_t peer_ip;
  uint16_t peer_port;
  uint8_t peer_mac[6];
  uint32_t relay_ip;
  uint16_t relay_port;
};
#pragma pack(pop)

int XdpLoad(XdpContext* ctx, const XdpConfig* config) {
  if (!ctx || !config) return -EINVAL;
  if (!config->interface) {
    spdlog::error("xdp: no interface specified");
    return -EINVAL;
  }

  // Resolve interface index.
  unsigned int ifindex = if_nametoindex(config->interface);
  if (ifindex == 0) {
    spdlog::error("xdp: interface '{}' not found: {}",
                  config->interface, strerror(errno));
    return -errno;
  }

  // Open BPF object file.
  const char* obj_path = config->bpf_obj_path;
  struct bpf_object* obj =
      bpf_object__open_file(obj_path, nullptr);
  if (!obj) {
    int err = -errno;
    spdlog::error("xdp: failed to open '{}': {}",
                  obj_path, strerror(-err));
    return err;
  }

  // Load BPF object into kernel.
  int ret = bpf_object__load(obj);
  if (ret < 0) {
    spdlog::error("xdp: failed to load BPF object: {}",
                  strerror(-ret));
    bpf_object__close(obj);
    return ret;
  }

  // Find the XDP program.
  struct bpf_program* prog =
      bpf_object__find_program_by_name(
          obj, "hd_stun_turn");
  if (!prog) {
    spdlog::error("xdp: program 'hd_stun_turn' not "
                  "found in object");
    bpf_object__close(obj);
    return -ENOENT;
  }
  int prog_fd = bpf_program__fd(prog);
  if (prog_fd < 0) {
    spdlog::error("xdp: invalid program fd");
    bpf_object__close(obj);
    return -ENOENT;
  }

  // Find maps.
  struct bpf_map* channels_map =
      bpf_object__find_map_by_name(obj, "turn_channels");
  struct bpf_map* stats_map =
      bpf_object__find_map_by_name(obj, "xdp_stats");
  struct bpf_map* port_map =
      bpf_object__find_map_by_name(obj, "stun_port");
  if (!channels_map || !stats_map || !port_map) {
    spdlog::error("xdp: required maps not found");
    bpf_object__close(obj);
    return -ENOENT;
  }

  int channels_fd = bpf_map__fd(channels_map);
  int stats_fd = bpf_map__fd(stats_map);
  int port_fd = bpf_map__fd(port_map);

  // Set the STUN port in the port map.
  uint32_t key0 = 0;
  uint16_t port_val = config->stun_port;
  ret = bpf_map_update_elem(port_fd, &key0, &port_val,
                            BPF_ANY);
  if (ret < 0) {
    spdlog::error("xdp: failed to set STUN port: {}",
                  strerror(-ret));
    bpf_object__close(obj);
    return ret;
  }

  // Attach XDP program. Try native (driver) mode first,
  // fall back to generic (SKB) mode.
  ret = bpf_xdp_attach(
      static_cast<int>(ifindex), prog_fd,
      XDP_FLAGS_DRV_MODE, nullptr);
  if (ret < 0) {
    spdlog::warn("xdp: native attach failed ({}), "
                 "trying generic mode",
                 strerror(-ret));
    ret = bpf_xdp_attach(
        static_cast<int>(ifindex), prog_fd,
        XDP_FLAGS_SKB_MODE, nullptr);
    if (ret < 0) {
      spdlog::error("xdp: generic attach failed: {}",
                    strerror(-ret));
      bpf_object__close(obj);
      return ret;
    }
    spdlog::info("xdp: attached in generic (SKB) mode "
                 "on {}", config->interface);
  } else {
    spdlog::info("xdp: attached in native (DRV) mode "
                 "on {}", config->interface);
  }

  ctx->prog_fd = prog_fd;
  ctx->ifindex = static_cast<int>(ifindex);
  ctx->channels_map_fd = channels_fd;
  ctx->stats_map_fd = stats_fd;
  ctx->port_map_fd = port_fd;
  ctx->bpf_obj = obj;
  ctx->attached = true;

  spdlog::info("xdp: loaded, STUN port={}, iface={}({})",
               config->stun_port, config->interface,
               ifindex);
  return 0;
}

void XdpUnload(XdpContext* ctx) {
  if (!ctx) return;

  if (ctx->attached && ctx->ifindex > 0) {
    int ret = bpf_xdp_detach(ctx->ifindex, 0, nullptr);
    if (ret < 0) {
      spdlog::warn("xdp: detach failed: {}",
                   strerror(-ret));
    } else {
      spdlog::info("xdp: detached from ifindex {}",
                   ctx->ifindex);
    }
  }

  if (ctx->bpf_obj) {
    bpf_object__close(
        static_cast<struct bpf_object*>(ctx->bpf_obj));
  }

  ctx->prog_fd = -1;
  ctx->ifindex = 0;
  ctx->channels_map_fd = -1;
  ctx->stats_map_fd = -1;
  ctx->port_map_fd = -1;
  ctx->bpf_obj = nullptr;
  ctx->attached = false;
}

int XdpUpdateChannel(XdpContext* ctx,
                     uint16_t channel,
                     uint32_t src_ip,
                     uint16_t src_port,
                     uint32_t peer_ip,
                     uint16_t peer_port,
                     const uint8_t* peer_mac,
                     uint32_t relay_ip,
                     uint16_t relay_port) {
  if (!ctx || ctx->channels_map_fd < 0) return -EINVAL;
  if (!peer_mac) return -EINVAL;

  TurnChanKey key{};
  key.channel = channel;
  key.src_ip = src_ip;
  key.src_port = src_port;

  TurnChanVal val{};
  val.peer_ip = peer_ip;
  val.peer_port = peer_port;
  std::memcpy(val.peer_mac, peer_mac, 6);
  val.relay_ip = relay_ip;
  val.relay_port = relay_port;

  int ret = bpf_map_update_elem(
      ctx->channels_map_fd, &key, &val, BPF_ANY);
  if (ret < 0) {
    spdlog::warn("xdp: channel update failed: {}",
                 strerror(-ret));
  }
  return ret;
}

int XdpRemoveChannel(XdpContext* ctx,
                     uint16_t channel,
                     uint32_t src_ip,
                     uint16_t src_port) {
  if (!ctx || ctx->channels_map_fd < 0) return -EINVAL;

  TurnChanKey key{};
  key.channel = channel;
  key.src_ip = src_ip;
  key.src_port = src_port;

  int ret = bpf_map_delete_elem(
      ctx->channels_map_fd, &key);
  if (ret < 0 && ret != -ENOENT) {
    spdlog::warn("xdp: channel remove failed: {}",
                 strerror(-ret));
  }
  return ret;
}

int XdpGetStats(XdpContext* ctx, XdpStats* stats) {
  if (!ctx || !stats || ctx->stats_map_fd < 0)
    return -EINVAL;

  int ncpus = libbpf_num_possible_cpus();
  if (ncpus < 0) return ncpus;

  // Per-CPU array: value buffer is ncpus * sizeof(uint64_t).
  auto values =
      std::make_unique<uint64_t[]>(
          static_cast<size_t>(ncpus));

  stats->stun_requests = 0;
  stats->stun_responses = 0;
  stats->channel_forwards = 0;
  stats->passed_to_stack = 0;

  uint64_t* sums[] = {
      &stats->stun_requests,
      &stats->stun_responses,
      &stats->channel_forwards,
      &stats->passed_to_stack,
  };

  for (uint32_t key = 0; key < 4; ++key) {
    int ret = bpf_map_lookup_elem(
        ctx->stats_map_fd, &key, values.get());
    if (ret < 0) {
      spdlog::warn("xdp: stats lookup key={} failed: {}",
                   key, strerror(-ret));
      continue;
    }
    for (int cpu = 0; cpu < ncpus; ++cpu) {
      *sums[key] += values[static_cast<size_t>(cpu)];
    }
  }

  return 0;
}

}  // namespace hyper_derp
