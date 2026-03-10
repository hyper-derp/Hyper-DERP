/// @file server.h
/// @brief DERP relay server: TCP listener, HTTP upgrade,
///   handshake, and data plane integration.

#ifndef INCLUDE_HYPER_DERP_SERVER_H_
#define INCLUDE_HYPER_DERP_SERVER_H_

#include <cstdint>
#include <pthread.h>

#include "hyper_derp/data_plane.h"
#include "hyper_derp/handshake.h"

namespace hyper_derp {

/// Server configuration.
struct ServerConfig {
  uint16_t port;
  int num_workers;  // 0 = auto (hardware_concurrency)
};

/// Top-level server state.
struct Server {
  ServerConfig config;
  ServerKeys keys;
  Ctx data_plane;
  int listen_fd;
  int running;
  pthread_t accept_thread;
  pthread_t control_thread;
};

/// @brief Initializes the server.
/// @param server Pointer to uninitialized Server.
/// @param config Server configuration.
/// @returns 0 on success, -1 on failure.
int ServerInit(Server* server,
               const ServerConfig* config);

/// @brief Starts the server (blocks until stopped).
/// @param server Initialized server.
/// @returns 0 on success, -1 on failure.
int ServerRun(Server* server);

/// @brief Signals the server to stop.
/// @param server Running server.
void ServerStop(Server* server);

/// @brief Tears down the server, freeing all resources.
/// @param server Server to destroy.
void ServerDestroy(Server* server);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_SERVER_H_
