/// @file internal.h
/// @brief SDK internal types shared between client and tunnel.

#ifndef HD_SDK_INTERNAL_H_
#define HD_SDK_INTERNAL_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "hd/sdk/config.h"
#include "hd/sdk/tunnel.h"

#include "hyper_derp/hd_client.h"

namespace hd::sdk {

struct Tunnel::Impl {
  std::string peer_name;
  uint16_t peer_id = 0;
  uint16_t relay_id = 0;  // 0 = local, >0 = remote relay.
  std::atomic<Mode> mode{Mode::Pending};
  hyper_derp::HdClient* hd = nullptr;
  std::mutex* send_mutex = nullptr;

  std::mutex cb_mutex;
  DataCallback on_data;
  ModeChangeCallback on_mode;
};

}  // namespace hd::sdk

#endif  // HD_SDK_INTERNAL_H_
