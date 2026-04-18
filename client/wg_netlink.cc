/// @file wg_netlink.cc
/// @brief WireGuard kernel configuration via generic netlink.

#include "wg_netlink.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <linux/genetlink.h>
#include <linux/rtnetlink.h>
#include <linux/wireguard.h>
#include <libmnl/libmnl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace hyper_derp {

// -- Family ID resolution ----------------------------------------------------

struct FamilyCtx {
  uint16_t id = 0;
  bool found = false;
};

static int FamilyCb(const struct nlattr* attr,
                    void* data) {
  auto* ctx = static_cast<FamilyCtx*>(data);
  if (mnl_attr_get_type(attr) == CTRL_ATTR_FAMILY_ID) {
    ctx->id = mnl_attr_get_u16(attr);
    ctx->found = true;
  }
  return MNL_CB_OK;
}

static int FamilyMsgCb(const struct nlmsghdr* nlh,
                       void* data) {
  mnl_attr_parse(nlh,
      sizeof(struct genlmsghdr), FamilyCb, data);
  return MNL_CB_OK;
}

auto WgNlInit(WgNetlink* wg)
    -> std::expected<void, Error<WgNlError>> {
  wg->nl = mnl_socket_open(NETLINK_GENERIC);
  if (!wg->nl) {
    return std::unexpected(MakeError(
        WgNlError::SocketFailed,
        "mnl_socket_open: " +
            std::string(strerror(errno))));
  }
  if (mnl_socket_bind(wg->nl, 0, MNL_SOCKET_AUTOPID) < 0) {
    mnl_socket_close(wg->nl);
    wg->nl = nullptr;
    return std::unexpected(MakeError(
        WgNlError::SocketFailed,
        "mnl_socket_bind: " +
            std::string(strerror(errno))));
  }
  wg->portid = mnl_socket_get_portid(wg->nl);

  // Resolve WireGuard family ID.
  uint8_t buf[MNL_SOCKET_BUFFER_SIZE];
  auto* nlh = mnl_nlmsg_put_header(buf);
  nlh->nlmsg_type = GENL_ID_CTRL;
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  nlh->nlmsg_seq = ++wg->seq;

  auto* genl = static_cast<struct genlmsghdr*>(
      mnl_nlmsg_put_extra_header(nlh,
          sizeof(struct genlmsghdr)));
  genl->cmd = CTRL_CMD_GETFAMILY;
  genl->version = 1;

  mnl_attr_put_strz(nlh, CTRL_ATTR_FAMILY_NAME,
                    WG_GENL_NAME);

  if (mnl_socket_sendto(wg->nl, nlh,
                        nlh->nlmsg_len) < 0) {
    mnl_socket_close(wg->nl);
    wg->nl = nullptr;
    return std::unexpected(MakeError(
        WgNlError::FamilyNotFound,
        "sendto CTRL_CMD_GETFAMILY: " +
            std::string(strerror(errno))));
  }

  int ret = mnl_socket_recvfrom(wg->nl, buf, sizeof(buf));
  if (ret < 0) {
    mnl_socket_close(wg->nl);
    wg->nl = nullptr;
    return std::unexpected(MakeError(
        WgNlError::FamilyNotFound,
        "recvfrom: " + std::string(strerror(errno))));
  }

  FamilyCtx ctx{};
  ret = mnl_cb_run(buf, ret, wg->seq, wg->portid,
                   FamilyMsgCb, &ctx);
  if (ret < 0 || !ctx.found) {
    mnl_socket_close(wg->nl);
    wg->nl = nullptr;
    return std::unexpected(MakeError(
        WgNlError::FamilyNotFound,
        "wireguard family not found (module loaded?)"));
  }
  wg->family_id = ctx.id;
  spdlog::info("wireguard netlink family id: {}",
               wg->family_id);
  return {};
}

// -- Device creation via rtnetlink -------------------------------------------

auto WgNlCreateDevice(const char* ifname)
    -> std::expected<void, Error<WgNlError>> {
  // Use rtnetlink to create the interface:
  // ip link add <ifname> type wireguard
  auto* nl = mnl_socket_open(NETLINK_ROUTE);
  if (!nl) {
    return std::unexpected(MakeError(
        WgNlError::CreateDeviceFailed,
        "mnl_socket_open(ROUTE): " +
            std::string(strerror(errno))));
  }
  if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
    mnl_socket_close(nl);
    return std::unexpected(MakeError(
        WgNlError::CreateDeviceFailed,
        "mnl_socket_bind: " +
            std::string(strerror(errno))));
  }

  uint8_t buf[MNL_SOCKET_BUFFER_SIZE];
  auto* nlh = mnl_nlmsg_put_header(buf);
  nlh->nlmsg_type = RTM_NEWLINK;
  nlh->nlmsg_flags =
      NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
  nlh->nlmsg_seq = 1;

  auto* ifi = static_cast<struct ifinfomsg*>(
      mnl_nlmsg_put_extra_header(nlh,
          sizeof(struct ifinfomsg)));
  ifi->ifi_family = AF_UNSPEC;

  mnl_attr_put_strz(nlh, IFLA_IFNAME, ifname);

  // Nested IFLA_LINKINFO → IFLA_INFO_KIND = "wireguard"
  auto* linkinfo = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
  mnl_attr_put_strz(nlh, IFLA_INFO_KIND, "wireguard");
  mnl_attr_nest_end(nlh, linkinfo);

  if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
    mnl_socket_close(nl);
    return std::unexpected(MakeError(
        WgNlError::CreateDeviceFailed,
        "sendto RTM_NEWLINK: " +
            std::string(strerror(errno))));
  }

  int ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
  mnl_socket_close(nl);

  if (ret < 0) {
    return std::unexpected(MakeError(
        WgNlError::CreateDeviceFailed,
        "recvfrom: " + std::string(strerror(errno))));
  }

  // Check for NLMSG_ERROR.
  auto* resp = reinterpret_cast<struct nlmsghdr*>(buf);
  if (resp->nlmsg_type == NLMSG_ERROR) {
    auto* err = static_cast<struct nlmsgerr*>(
        mnl_nlmsg_get_payload(resp));
    if (err->error != 0 && err->error != -EEXIST) {
      return std::unexpected(MakeError(
          WgNlError::CreateDeviceFailed,
          "RTM_NEWLINK: " +
              std::string(strerror(-err->error))));
    }
  }

  spdlog::info("wireguard interface {} created", ifname);
  return {};
}

// -- Device configuration ----------------------------------------------------

/// Context for passing netlink error code out of callback.
struct NlAckCtx {
  int error = 0;
};

static int AckCb(const struct nlmsghdr* nlh, void* data) {
  auto* ctx = static_cast<NlAckCtx*>(data);
  if (nlh->nlmsg_type == NLMSG_ERROR) {
    auto* err = static_cast<struct nlmsgerr*>(
        mnl_nlmsg_get_payload(nlh));
    ctx->error = err->error;
    if (err->error != 0) return MNL_CB_ERROR;
  }
  return MNL_CB_OK;
}

auto WgNlSetDevice(WgNetlink* wg, const char* ifname,
                   const uint8_t* private_key,
                   uint16_t listen_port)
    -> std::expected<void, Error<WgNlError>> {
  uint8_t buf[MNL_SOCKET_BUFFER_SIZE];
  auto* nlh = mnl_nlmsg_put_header(buf);
  nlh->nlmsg_type = wg->family_id;
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  nlh->nlmsg_seq = ++wg->seq;

  auto* genl = static_cast<struct genlmsghdr*>(
      mnl_nlmsg_put_extra_header(nlh,
          sizeof(struct genlmsghdr)));
  genl->cmd = WG_CMD_SET_DEVICE;
  genl->version = 1;

  mnl_attr_put_strz(nlh, WGDEVICE_A_IFNAME, ifname);
  mnl_attr_put(nlh, WGDEVICE_A_PRIVATE_KEY,
               kWgKeySize, private_key);
  mnl_attr_put_u16(nlh, WGDEVICE_A_LISTEN_PORT,
                   listen_port);

  if (mnl_socket_sendto(wg->nl, nlh,
                        nlh->nlmsg_len) < 0) {
    return std::unexpected(MakeError(
        WgNlError::SetDeviceFailed,
        "sendto: " + std::string(strerror(errno))));
  }

  int ret = mnl_socket_recvfrom(wg->nl, buf, sizeof(buf));
  if (ret < 0) {
    return std::unexpected(MakeError(
        WgNlError::SetDeviceFailed,
        "recvfrom: " + std::string(strerror(errno))));
  }

  NlAckCtx ack_ctx{};
  ret = mnl_cb_run(buf, ret, wg->seq, wg->portid,
                   AckCb, &ack_ctx);
  if (ret < 0 && ack_ctx.error != 0) {
    return std::unexpected(MakeError(
        WgNlError::SetDeviceFailed,
        "set device: " +
            std::string(strerror(-ack_ctx.error))));
  }

  spdlog::info("wireguard {} configured (port {})",
               ifname, listen_port);
  return {};
}

// -- Peer management ---------------------------------------------------------

auto WgNlSetPeer(WgNetlink* wg, const char* ifname,
                 const WgPeerConfig* peer)
    -> std::expected<void, Error<WgNlError>> {
  uint8_t buf[MNL_SOCKET_BUFFER_SIZE];
  auto* nlh = mnl_nlmsg_put_header(buf);
  nlh->nlmsg_type = wg->family_id;
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  nlh->nlmsg_seq = ++wg->seq;

  auto* genl = static_cast<struct genlmsghdr*>(
      mnl_nlmsg_put_extra_header(nlh,
          sizeof(struct genlmsghdr)));
  genl->cmd = WG_CMD_SET_DEVICE;
  genl->version = 1;

  mnl_attr_put_strz(nlh, WGDEVICE_A_IFNAME, ifname);

  // Replace existing allowed IPs for this peer.
  uint32_t flags = WGDEVICE_F_REPLACE_PEERS;
  // Actually, we don't want to replace ALL peers, just
  // update this one. Use per-peer REPLACE_ALLOWEDIPS.

  // Nested: WGDEVICE_A_PEERS → WGPEER_A_*
  auto* peers_nest = mnl_attr_nest_start(nlh,
      WGDEVICE_A_PEERS);
  auto* peer_nest = mnl_attr_nest_start(nlh, 0);

  mnl_attr_put(nlh, WGPEER_A_PUBLIC_KEY,
               kWgKeySize, peer->public_key);

  if (peer->endpoint_ip != 0 && peer->endpoint_port != 0) {
    struct sockaddr_in ep{};
    ep.sin_family = AF_INET;
    ep.sin_addr.s_addr = peer->endpoint_ip;
    ep.sin_port = peer->endpoint_port;
    mnl_attr_put(nlh, WGPEER_A_ENDPOINT,
                 sizeof(ep), &ep);
  }

  mnl_attr_put_u16(nlh,
      WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL,
      peer->keepalive_secs);

  uint32_t peer_flags = WGPEER_F_REPLACE_ALLOWEDIPS;
  mnl_attr_put_u32(nlh, WGPEER_A_FLAGS, peer_flags);

  // Nested allowed IPs.
  auto* aips_nest = mnl_attr_nest_start(nlh,
      WGPEER_A_ALLOWEDIPS);
  auto* aip_nest = mnl_attr_nest_start(nlh, 0);
  mnl_attr_put_u16(nlh, WGALLOWEDIP_A_FAMILY, AF_INET);
  mnl_attr_put(nlh, WGALLOWEDIP_A_IPADDR, 4,
               &peer->allowed_ip);
  mnl_attr_put_u8(nlh, WGALLOWEDIP_A_CIDR_MASK,
                  static_cast<uint8_t>(peer->allowed_prefix));
  mnl_attr_nest_end(nlh, aip_nest);
  mnl_attr_nest_end(nlh, aips_nest);

  mnl_attr_nest_end(nlh, peer_nest);
  mnl_attr_nest_end(nlh, peers_nest);

  if (mnl_socket_sendto(wg->nl, nlh,
                        nlh->nlmsg_len) < 0) {
    return std::unexpected(MakeError(
        WgNlError::SetPeerFailed,
        "sendto: " + std::string(strerror(errno))));
  }

  int ret = mnl_socket_recvfrom(wg->nl, buf, sizeof(buf));
  if (ret < 0) {
    return std::unexpected(MakeError(
        WgNlError::SetPeerFailed,
        "recvfrom: " + std::string(strerror(errno))));
  }

  NlAckCtx ack_ctx{};
  ret = mnl_cb_run(buf, ret, wg->seq, wg->portid,
                   AckCb, &ack_ctx);
  if (ret < 0 && ack_ctx.error != 0) {
    return std::unexpected(MakeError(
        WgNlError::SetPeerFailed,
        "set peer: " +
            std::string(strerror(-ack_ctx.error))));
  }

  return {};
}

auto WgNlRemovePeer(WgNetlink* wg, const char* ifname,
                    const uint8_t* public_key)
    -> std::expected<void, Error<WgNlError>> {
  uint8_t buf[MNL_SOCKET_BUFFER_SIZE];
  auto* nlh = mnl_nlmsg_put_header(buf);
  nlh->nlmsg_type = wg->family_id;
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  nlh->nlmsg_seq = ++wg->seq;

  auto* genl = static_cast<struct genlmsghdr*>(
      mnl_nlmsg_put_extra_header(nlh,
          sizeof(struct genlmsghdr)));
  genl->cmd = WG_CMD_SET_DEVICE;
  genl->version = 1;

  mnl_attr_put_strz(nlh, WGDEVICE_A_IFNAME, ifname);

  auto* peers_nest = mnl_attr_nest_start(nlh,
      WGDEVICE_A_PEERS);
  auto* peer_nest = mnl_attr_nest_start(nlh, 0);
  mnl_attr_put(nlh, WGPEER_A_PUBLIC_KEY,
               kWgKeySize, public_key);
  mnl_attr_put_u32(nlh, WGPEER_A_FLAGS,
                   WGPEER_F_REMOVE_ME);
  mnl_attr_nest_end(nlh, peer_nest);
  mnl_attr_nest_end(nlh, peers_nest);

  if (mnl_socket_sendto(wg->nl, nlh,
                        nlh->nlmsg_len) < 0) {
    return std::unexpected(MakeError(
        WgNlError::RemovePeerFailed,
        "sendto: " + std::string(strerror(errno))));
  }

  int ret = mnl_socket_recvfrom(wg->nl, buf, sizeof(buf));
  if (ret < 0) {
    return std::unexpected(MakeError(
        WgNlError::RemovePeerFailed,
        "recvfrom: " + std::string(strerror(errno))));
  }

  NlAckCtx ack_ctx{};
  ret = mnl_cb_run(buf, ret, wg->seq, wg->portid,
                   AckCb, &ack_ctx);
  if (ret < 0 && ack_ctx.error != 0) {
    return std::unexpected(MakeError(
        WgNlError::RemovePeerFailed,
        "remove peer: " +
            std::string(strerror(-ack_ctx.error))));
  }

  return {};
}

// -- Interface address (ioctl, same pattern as tun.cc) -----------------------

auto WgNlConfigureAddr(const char* ifname,
                       uint32_t addr, int prefix_len)
    -> std::expected<void, Error<WgNlError>> {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    return std::unexpected(MakeError(
        WgNlError::InterfaceFailed,
        "socket: " + std::string(strerror(errno))));
  }

  struct ifreq ifr{};
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

  // Set address.
  auto* sa = reinterpret_cast<struct sockaddr_in*>(
      &ifr.ifr_addr);
  sa->sin_family = AF_INET;
  sa->sin_addr.s_addr = addr;
  if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
    close(sock);
    return std::unexpected(MakeError(
        WgNlError::InterfaceFailed,
        "SIOCSIFADDR: " +
            std::string(strerror(errno))));
  }

  // Set netmask.
  sa->sin_addr.s_addr =
      htonl(~((1u << (32 - prefix_len)) - 1));
  if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0) {
    close(sock);
    return std::unexpected(MakeError(
        WgNlError::InterfaceFailed,
        "SIOCSIFNETMASK: " +
            std::string(strerror(errno))));
  }

  // Bring up.
  if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
    close(sock);
    return std::unexpected(MakeError(
        WgNlError::InterfaceFailed,
        "SIOCGIFFLAGS: " +
            std::string(strerror(errno))));
  }
  ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
  if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
    close(sock);
    return std::unexpected(MakeError(
        WgNlError::InterfaceFailed,
        "SIOCSIFFLAGS: " +
            std::string(strerror(errno))));
  }

  close(sock);
  spdlog::info("wireguard {} addr {}/{} up", ifname,
               inet_ntoa({addr}), prefix_len);
  return {};
}

void WgNlClose(WgNetlink* wg) {
  if (wg->nl) {
    mnl_socket_close(wg->nl);
    wg->nl = nullptr;
  }
}

}  // namespace hyper_derp
