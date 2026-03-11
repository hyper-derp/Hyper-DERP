/// @file error.h
/// @brief Generic error template for std::expected returns.
///
/// Domain-specific error enums live in their own headers
/// (http.h, handshake.h, server.h, client.h). The data plane
/// is excluded — it uses raw return codes for zero overhead.

#ifndef INCLUDE_HYPER_DERP_ERROR_H_
#define INCLUDE_HYPER_DERP_ERROR_H_

#include <expected>
#include <string>
#include <utility>

namespace hyper_derp {

/// Generic error wrapper parameterized by an error code enum.
template <typename ErrorCodeEnum>
struct Error {
  ErrorCodeEnum code;
  std::string message;
};

/// Construct an Error<E> in-place.
template <typename E>
auto MakeError(E code, std::string message)
    -> std::unexpected<Error<E>> {
  return std::unexpected(
      Error<E>{code, std::move(message)});
}

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_ERROR_H_
