/// @file ctl_channel.h
/// @brief ZMQ IPC control channel for hdctl.
///
/// ROUTER socket on ipc:///tmp/hyper-derp.sock.
/// JSON request/response, same data as REST API.

#ifndef INCLUDE_HYPER_DERP_CTL_CHANNEL_H_
#define INCLUDE_HYPER_DERP_CTL_CHANNEL_H_

#include "hyper_derp/hd_peers.h"
#include "hyper_derp/types.h"

namespace hyper_derp {

struct CtlChannel;

/// Start the ZMQ IPC control channel.
/// @param ipc_path IPC endpoint (e.g. "ipc:///tmp/hyper-derp.sock")
/// @param ctx Data plane context.
/// @param hd_peers HD peer registry (may be nullptr).
CtlChannel* CtlChannelStart(const char* ipc_path,
                            Ctx* ctx,
                            HdPeerRegistry* hd_peers);

/// Stop and free.
void CtlChannelStop(CtlChannel* ch);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_CTL_CHANNEL_H_
