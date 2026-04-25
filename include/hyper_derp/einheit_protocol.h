/// @file einheit_protocol.h
/// @brief Einheit wire-protocol envelopes + MessagePack codec.
///
/// Mirrors einheit-cli's `protocol/envelope.h` +
/// `protocol/msgpack_codec.h` byte-for-byte on the wire. Kept
/// copy-local (rather than depending on the framework repo) so
/// the daemon can ship standalone; round-trip tests on both sides
/// catch any drift before it hits an operator.
///
/// When a second product daemon gains einheit-protocol support the
/// expectation is to extract this into a standalone
/// `einheit-protocol` repo and have both consumers depend on it.

#ifndef INCLUDE_HYPER_DERP_EINHEIT_PROTOCOL_H_
#define INCLUDE_HYPER_DERP_EINHEIT_PROTOCOL_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "hyper_derp/error.h"

namespace hyper_derp::einheit {

/// Current envelope schema version. Bump on incompatible changes.
inline constexpr std::uint32_t kEnvelopeVersion = 1;

/// Request envelope — CLI to daemon on the .ctl socket.
struct Request {
  std::uint32_t envelope_version = kEnvelopeVersion;
  std::string id;
  std::string user;
  std::string role;
  std::optional<std::string> session_id;
  std::string command;
  std::vector<std::string> args;
  std::unordered_map<std::string, std::string> flags;
};

/// Response status discriminator.
enum class ResponseStatus {
  kOk,
  kError,
};

/// Structured error body inside a Response.
struct ResponseError {
  std::string code;
  std::string message;
  std::string hint;
};

/// Response envelope — daemon to CLI on the .ctl socket.
struct Response {
  std::uint32_t envelope_version = kEnvelopeVersion;
  std::string id;
  ResponseStatus status = ResponseStatus::kOk;
  /// Command-specific payload. Encoded as MessagePack by the
  /// daemon handlers and decoded by the adapter's renderers.
  std::vector<std::uint8_t> data;
  std::optional<ResponseError> error;
};

/// Event envelope — daemon to subscribers on the .pub socket.
/// Arrives as two ZMQ frames: topic (UTF-8 string) + data (msgpack).
struct Event {
  std::string topic;
  std::uint32_t envelope_version = kEnvelopeVersion;
  std::string timestamp;
  std::vector<std::uint8_t> data;
};

/// Codec error surfaces for both encode and decode.
enum class CodecError {
  kTruncated,
  kTypeMismatch,
  kVersionMismatch,
  kMissingField,
  kExceptionRaised,
};

/// Encode a Request to MessagePack bytes.
auto EncodeRequest(const Request& req)
    -> std::expected<std::vector<std::uint8_t>,
                     Error<CodecError>>;

/// Decode a MessagePack buffer into a Request.
auto DecodeRequest(std::span<const std::uint8_t> bytes)
    -> std::expected<Request, Error<CodecError>>;

/// Encode a Response to MessagePack bytes.
auto EncodeResponse(const Response& res)
    -> std::expected<std::vector<std::uint8_t>,
                     Error<CodecError>>;

/// Decode a MessagePack buffer into a Response.
auto DecodeResponse(std::span<const std::uint8_t> bytes)
    -> std::expected<Response, Error<CodecError>>;

/// Encode an Event body (topic rides as a separate ZMQ frame).
auto EncodeEventBody(const Event& ev)
    -> std::expected<std::vector<std::uint8_t>,
                     Error<CodecError>>;

/// Decode an Event body given the topic (first frame) + body.
auto DecodeEventBody(const std::string& topic,
                     std::span<const std::uint8_t> bytes)
    -> std::expected<Event, Error<CodecError>>;

}  // namespace hyper_derp::einheit

#endif  // INCLUDE_HYPER_DERP_EINHEIT_PROTOCOL_H_
