/// @file error.h
/// @brief SDK error types.

#ifndef HD_SDK_ERROR_H_
#define HD_SDK_ERROR_H_

#include <cstdint>
#include <expected>
#include <string>

namespace hd::sdk {

enum class ErrorCode {
  kOk = 0,
  kInitFailed,
  kConnectFailed,
  kAlreadyRunning,
  kNotRunning,
  kNotConnected,
  kSendFailed,
  kPeerNotFound,
  kPolicyDenied,
  kNetworkError,
  kTimeout,
  kPoolExhausted,
};

struct Error {
  ErrorCode code;
  std::string message;
};

template <typename T = void>
using Result = std::expected<T, Error>;

inline Error MakeError(ErrorCode code,
                       std::string msg) {
  return {code, std::move(msg)};
}

}  // namespace hd::sdk

#endif  // HD_SDK_ERROR_H_
