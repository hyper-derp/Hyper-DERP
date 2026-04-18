/// @file ice_agent.cc
/// @brief ICE agent implementation using relay's stun.h.

#include "hd/ice/ice_agent.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hyper_derp/stun.h"

namespace hd::ice {

struct IceAgent::Impl {
  IceConfig config;
  int stun_fd = -1;
  uint32_t reflexive_ip = 0;
  uint16_t reflexive_port = 0;
  NominationCallback on_nomination;
};

IceAgent::IceAgent(const IceConfig& config)
    : impl_(new Impl()) {
  impl_->config = config;
}

IceAgent::~IceAgent() {
  CloseStunSocket();
  delete impl_;
}

hd::sdk::Result<> IceAgent::GatherCandidates(
    CandidateCallback cb) {
  std::vector<Candidate> candidates;

  // Host candidates: discover local IP.
  {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
      sockaddr_in dst{};
      dst.sin_family = AF_INET;
      dst.sin_addr.s_addr = inet_addr("8.8.8.8");
      dst.sin_port = htons(53);
      if (connect(s, reinterpret_cast<sockaddr*>(&dst),
                  sizeof(dst)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        getsockname(s,
            reinterpret_cast<sockaddr*>(&local), &len);
        Candidate c{};
        c.type = Candidate::kHost;
        c.ip = local.sin_addr.s_addr;
        c.port = htons(impl_->config.local_port);
        candidates.push_back(c);
      }
      close(s);
    }
  }

  // STUN: bind on local_port, send to each server.
  for (const auto& server : impl_->config.stun_servers) {
    if (impl_->stun_fd < 0) {
      impl_->stun_fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (impl_->stun_fd < 0) continue;

      int reuse = 1;
      setsockopt(impl_->stun_fd, SOL_SOCKET,
                 SO_REUSEPORT, &reuse, sizeof(reuse));

      sockaddr_in local{};
      local.sin_family = AF_INET;
      local.sin_addr.s_addr = INADDR_ANY;
      local.sin_port = htons(impl_->config.local_port);
      if (bind(impl_->stun_fd,
               reinterpret_cast<sockaddr*>(&local),
               sizeof(local)) < 0) {
        close(impl_->stun_fd);
        impl_->stun_fd = -1;
        continue;
      }
    }

    // Parse server address.
    std::string host = server;
    uint16_t port = 3478;
    auto colon = host.rfind(':');
    if (colon != std::string::npos) {
      port = static_cast<uint16_t>(
          atoi(host.c_str() + colon + 1));
      host = host.substr(0, colon);
    }

    sockaddr_in stun_addr{};
    stun_addr.sin_family = AF_INET;
    stun_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(),
                  &stun_addr.sin_addr) != 1) {
      addrinfo hints{};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;
      addrinfo* res = nullptr;
      if (getaddrinfo(host.c_str(), nullptr,
                      &hints, &res) == 0 && res) {
        stun_addr.sin_addr =
            reinterpret_cast<sockaddr_in*>(
                res->ai_addr)->sin_addr;
        freeaddrinfo(res);
      } else {
        continue;
      }
    }

    uint8_t req[64];
    int req_len = hyper_derp::StunBuildBindingRequest(
        req, sizeof(req), nullptr);
    if (req_len <= 0) continue;

    sendto(impl_->stun_fd, req, req_len, 0,
           reinterpret_cast<sockaddr*>(&stun_addr),
           sizeof(stun_addr));

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(impl_->stun_fd, SOL_SOCKET, SO_RCVTIMEO,
               &tv, sizeof(tv));

    uint8_t resp[256];
    int n = recv(impl_->stun_fd, resp, sizeof(resp), 0);
    if (n <= 0) continue;

    hyper_derp::StunMessage msg{};
    if (hyper_derp::StunParse(resp, n, &msg) &&
        msg.has_xor_mapped) {
      hyper_derp::StunDecodeXorAddress(
          msg.xor_mapped_port, msg.xor_mapped_ip,
          &impl_->reflexive_port, &impl_->reflexive_ip);

      Candidate c{};
      c.type = Candidate::kServerReflexive;
      c.ip = impl_->reflexive_ip;
      c.port = impl_->reflexive_port;
      candidates.push_back(c);
    }
  }

  if (cb) cb(candidates);
  return {};
}

void IceAgent::OnNomination(NominationCallback cb) {
  impl_->on_nomination = std::move(cb);
}

void IceAgent::AddRemoteCandidates(
    const std::vector<Candidate>& candidates) {
  // For each remote candidate, try to nominate.
  // In the simple model: first reachable candidate wins.
  if (!candidates.empty() && impl_->on_nomination) {
    // Prefer host candidates (same LAN).
    for (const auto& c : candidates) {
      if (c.type == Candidate::kHost) {
        impl_->on_nomination(c.ip, c.port);
        return;
      }
    }
    // Fall back to srflx.
    for (const auto& c : candidates) {
      if (c.type == Candidate::kServerReflexive) {
        impl_->on_nomination(c.ip, c.port);
        return;
      }
    }
  }
}

int IceAgent::StunFd() const {
  return impl_ ? impl_->stun_fd : -1;
}

void IceAgent::CloseStunSocket() {
  if (impl_ && impl_->stun_fd >= 0) {
    close(impl_->stun_fd);
    impl_->stun_fd = -1;
  }
}

}  // namespace hd::ice
