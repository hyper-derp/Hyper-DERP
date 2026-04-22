/// @file einheit_channel.h
/// @brief ZMQ control + event channel speaking the einheit
///   protocol. Runs alongside the legacy JSON ctl_channel so
///   hdctl keeps working during the transition.

#ifndef INCLUDE_HYPER_DERP_EINHEIT_CHANNEL_H_
#define INCLUDE_HYPER_DERP_EINHEIT_CHANNEL_H_

#include <string>

namespace hyper_derp {

struct Server;
struct EinheitChannel;

/// Start the einheit control + event channel.
/// @param ctl_endpoint ZMQ endpoint for the ROUTER socket
///   (e.g. "ipc:///var/run/einheit/hd-relay.ctl").
/// @param pub_endpoint ZMQ endpoint for the PUB socket
///   (e.g. "ipc:///var/run/einheit/hd-relay.pub"). Empty
///   disables event publishing.
/// @param server Server used by handlers for state access.
/// @returns Channel handle, or nullptr on bind failure.
EinheitChannel* EinheitChannelStart(
    const std::string& ctl_endpoint,
    const std::string& pub_endpoint, Server* server);

/// Stop and free the channel.
void EinheitChannelStop(EinheitChannel* ch);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_EINHEIT_CHANNEL_H_
