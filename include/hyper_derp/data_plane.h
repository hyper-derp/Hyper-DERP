/// @file data_plane.h
/// @brief io_uring data plane: init, run, teardown, and
///   control-plane command interface.

#ifndef INCLUDE_HYPER_DERP_DATA_PLANE_H_
#define INCLUDE_HYPER_DERP_DATA_PLANE_H_

#include <cstdint>

#include "hyper_derp/types.h"

namespace hyper_derp {

/// @brief Initializes the data plane context.
/// @param ctx Pointer to an uninitialized Ctx.
/// @param num_workers Number of worker threads to create.
/// @returns 0 on success, -1 on failure.
int DpInit(Ctx* ctx, int num_workers);

/// @brief Starts all worker threads and blocks until they
///   exit.
/// @param ctx Initialized context.
/// @returns 0 on success, -1 if thread creation fails.
int DpRun(Ctx* ctx);

/// @brief Tears down the data plane, freeing all resources.
/// @param ctx Context to destroy.
void DpDestroy(Ctx* ctx);

/// @brief Registers a new peer with the data plane.
/// @param ctx Data plane context.
/// @param fd The peer's socket file descriptor.
/// @param key The peer's 32-byte public key.
/// @param protocol Protocol type (DERP or HD).
void DpAddPeer(Ctx* ctx, int fd, const Key& key,
               PeerProtocol protocol =
                   PeerProtocol::kDerp);

/// @brief Removes a peer from the data plane.
/// @param ctx Data plane context.
/// @param key The peer's 32-byte public key.
void DpRemovePeer(Ctx* ctx, const Key& key);

/// @brief Sends data to a peer (from the control plane).
/// @param ctx Data plane context.
/// @param key The destination peer's 32-byte public key.
/// @param data Pointer to data (ownership transferred).
/// @param data_len Length of data.
void DpWrite(Ctx* ctx, const Key& key,
             uint8_t* data, int data_len);

/// @brief Signals all workers to stop.
/// @param ctx Data plane context.
void DpStop(Ctx* ctx);

/// @brief Aggregates statistics from all workers.
/// @param ctx Data plane context.
/// @param send_drops Output: total send drops.
/// @param xfer_drops Output: total cross-shard drops.
/// @param slab_exhausts Output: total slab exhaustions.
/// @param recv_bytes Output: total bytes received.
/// @param send_bytes Output: total bytes sent.
void DpGetStats(Ctx* ctx,
                uint64_t* send_drops,
                uint64_t* xfer_drops,
                uint64_t* slab_exhausts,
                uint64_t* recv_bytes,
                uint64_t* send_bytes);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_DATA_PLANE_H_
