/// @file hd_sdk_c.cc
/// @brief C ABI wrapper implementation.

#include "hd_sdk_c.h"

#include <cstring>

#include <hd/sdk.hpp>

using namespace hd::sdk;

// Thread-local error message.
static thread_local char g_error[512] = {};

static void SetError(const char* msg) {
  strncpy(g_error, msg, sizeof(g_error) - 1);
  g_error[sizeof(g_error) - 1] = '\0';
}

const char* hd_last_error(void) {
  return g_error;
}

// -- Client ------------------------------------------------------------------

struct hd_client {
  std::unique_ptr<Client> cpp;
  hd_peer_callback_t peer_cb = nullptr;
  void* peer_ud = nullptr;
};

hd_client_t* hd_client_create(
    const hd_client_config_t* config) {
  ClientConfig cfg;
  if (config->relay_url)
    cfg.relay_url = config->relay_url;
  if (config->relay_key)
    cfg.relay_key = config->relay_key;
  if (config->key_path)
    cfg.key_path = config->key_path;
  if (config->keepalive_secs > 0)
    cfg.keepalive =
        std::chrono::seconds(config->keepalive_secs);
  cfg.tls = config->tls != 0;
  cfg.auto_reconnect = config->auto_reconnect != 0;

  auto result = Client::Create(cfg);
  if (!result) {
    SetError(result.error().message.c_str());
    return nullptr;
  }

  auto* c = new hd_client();
  c->cpp = std::make_unique<Client>(std::move(*result));
  return c;
}

int hd_client_start(hd_client_t* c) {
  if (!c) return HD_ERR_INIT;
  auto r = c->cpp->Start();
  if (!r) {
    SetError(r.error().message.c_str());
    return HD_ERR_RUNNING;
  }
  return HD_OK;
}

void hd_client_stop(hd_client_t* c) {
  if (c) c->cpp->Stop();
}

void hd_client_destroy(hd_client_t* c) {
  delete c;
}

int hd_client_connected(const hd_client_t* c) {
  return c && c->cpp->GetStatus() == Status::Connected
      ? 1 : 0;
}

void hd_client_set_peer_callback(
    hd_client_t* c, hd_peer_callback_t cb,
    void* userdata) {
  if (!c) return;
  c->peer_cb = cb;
  c->peer_ud = userdata;
  if (cb) {
    c->cpp->SetPeerCallback(
        [c](const PeerInfo& p, bool conn) {
      if (c->peer_cb) {
        c->peer_cb(p.public_key.data(), p.peer_id,
                   conn ? 1 : 0, c->peer_ud);
      }
    });
  } else {
    c->cpp->SetPeerCallback(nullptr);
  }
}

// -- Tunnels -----------------------------------------------------------------

struct hd_tunnel {
  std::unique_ptr<Tunnel> cpp;
  hd_data_callback_t data_cb = nullptr;
  void* data_ud = nullptr;
};

hd_tunnel_t* hd_client_open(hd_client_t* c,
                            const char* peer_name) {
  if (!c || !peer_name) return nullptr;
  auto r = c->cpp->Open(peer_name);
  if (!r) {
    SetError(r.error().message.c_str());
    return nullptr;
  }
  auto* t = new hd_tunnel();
  t->cpp = std::make_unique<Tunnel>(std::move(*r));
  return t;
}

int hd_tunnel_send(hd_tunnel_t* t,
                   const uint8_t* data, int len) {
  if (!t) return HD_ERR_INIT;
  auto r = t->cpp->Send(
      std::span<const uint8_t>(data, len));
  if (!r) {
    SetError(r.error().message.c_str());
    return HD_ERR_SEND;
  }
  return HD_OK;
}

void hd_tunnel_set_data_callback(
    hd_tunnel_t* t, hd_data_callback_t cb,
    void* userdata) {
  if (!t) return;
  t->data_cb = cb;
  t->data_ud = userdata;
  if (cb) {
    t->cpp->SetDataCallback(
        [t](std::span<const uint8_t> data) {
      if (t->data_cb) {
        t->data_cb(0, data.data(),
                   static_cast<int>(data.size()),
                   t->data_ud);
      }
    });
  } else {
    t->cpp->SetDataCallback(nullptr);
  }
}

void hd_tunnel_close(hd_tunnel_t* t) {
  if (t) t->cpp->Close();
}

void hd_tunnel_destroy(hd_tunnel_t* t) {
  delete t;
}

const char* hd_tunnel_peer_name(const hd_tunnel_t* t) {
  return t ? t->cpp->PeerName().c_str() : "";
}

uint16_t hd_tunnel_peer_id(const hd_tunnel_t* t) {
  return t ? t->cpp->PeerId() : 0;
}

int hd_tunnel_mode(const hd_tunnel_t* t) {
  if (!t) return 3;
  return static_cast<int>(t->cpp->CurrentMode());
}

// -- Raw MeshData ------------------------------------------------------------

int hd_client_send_mesh(hd_client_t* c,
                        uint16_t dst_peer_id,
                        const uint8_t* data, int len) {
  if (!c) return HD_ERR_INIT;
  auto r = c->cpp->SendMeshData(
      dst_peer_id,
      std::span<const uint8_t>(data, len));
  if (!r) {
    SetError(r.error().message.c_str());
    return HD_ERR_SEND;
  }
  return HD_OK;
}
