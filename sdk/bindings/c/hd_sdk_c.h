/// @file hd_sdk_c.h
/// @brief C ABI wrapper for the HD SDK.
///
/// Every C++ method has a corresponding C function.
/// Errors are returned as negative codes with an
/// optional message via hd_last_error().

#ifndef HD_SDK_C_H_
#define HD_SDK_C_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -- Opaque handles ----------------------------------------------------------

typedef struct hd_client hd_client_t;
typedef struct hd_tunnel hd_tunnel_t;

// -- Error codes -------------------------------------------------------------

#define HD_OK             0
#define HD_ERR_INIT      -1
#define HD_ERR_CONNECT   -2
#define HD_ERR_RUNNING   -3
#define HD_ERR_SEND      -4
#define HD_ERR_NOT_FOUND -5
#define HD_ERR_POLICY    -6
#define HD_ERR_NETWORK   -7
#define HD_ERR_TIMEOUT   -8

/// Get the last error message (thread-local).
const char* hd_last_error(void);

// -- Client ------------------------------------------------------------------

typedef struct {
  const char* relay_url;
  const char* relay_key;
  const char* key_path;
  int keepalive_secs;
  int tls;
  int auto_reconnect;
} hd_client_config_t;

/// Create and connect a client.
hd_client_t* hd_client_create(
    const hd_client_config_t* config);

/// Start the I/O thread.
int hd_client_start(hd_client_t* c);

/// Stop and disconnect.
void hd_client_stop(hd_client_t* c);

/// Destroy the client (calls stop if needed).
void hd_client_destroy(hd_client_t* c);

/// Get connection status (0=disconnected, 1=connected).
int hd_client_connected(const hd_client_t* c);

// -- Callbacks ---------------------------------------------------------------

typedef void (*hd_peer_callback_t)(
    const uint8_t* key, uint16_t peer_id,
    int connected, void* userdata);
typedef void (*hd_data_callback_t)(
    uint16_t src_peer_id,
    const uint8_t* data, int len,
    void* userdata);

void hd_client_set_peer_callback(
    hd_client_t* c, hd_peer_callback_t cb,
    void* userdata);

// -- Tunnels -----------------------------------------------------------------

/// Open a tunnel to a named peer. Returns NULL on failure.
hd_tunnel_t* hd_client_open(hd_client_t* c,
                            const char* peer_name);

/// Send data on a tunnel.
int hd_tunnel_send(hd_tunnel_t* t,
                   const uint8_t* data, int len);

/// Set data callback on a tunnel.
void hd_tunnel_set_data_callback(
    hd_tunnel_t* t, hd_data_callback_t cb,
    void* userdata);

/// Close a tunnel.
void hd_tunnel_close(hd_tunnel_t* t);

/// Destroy a tunnel.
void hd_tunnel_destroy(hd_tunnel_t* t);

/// Get peer name.
const char* hd_tunnel_peer_name(const hd_tunnel_t* t);

/// Get peer ID.
uint16_t hd_tunnel_peer_id(const hd_tunnel_t* t);

/// Get mode (0=pending, 1=direct, 2=relayed, 3=closed).
int hd_tunnel_mode(const hd_tunnel_t* t);

// -- Raw MeshData ------------------------------------------------------------

int hd_client_send_mesh(hd_client_t* c,
                        uint16_t dst_peer_id,
                        const uint8_t* data, int len);

#ifdef __cplusplus
}
#endif

#endif  // HD_SDK_C_H_
