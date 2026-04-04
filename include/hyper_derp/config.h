/// @file config.h
/// @brief YAML configuration file loader.

#ifndef INCLUDE_HYPER_DERP_CONFIG_H_
#define INCLUDE_HYPER_DERP_CONFIG_H_

#include <expected>
#include <string>

#include "hyper_derp/error.h"
#include "hyper_derp/server.h"

namespace hyper_derp {

/// Error codes for config loading.
enum class ConfigError {
  FileNotFound,
  ReadFailed,
  ParseFailed,
  InvalidValue,
};

/// Human-readable name for a ConfigError code.
constexpr auto ConfigErrorName(ConfigError e)
    -> std::string_view {
  switch (e) {
    case ConfigError::FileNotFound:
      return "FileNotFound";
    case ConfigError::ReadFailed:
      return "ReadFailed";
    case ConfigError::ParseFailed:
      return "ParseFailed";
    case ConfigError::InvalidValue:
      return "InvalidValue";
  }
  return "Unknown";
}

/// @brief Load a ServerConfig from a YAML file.
/// @param path Path to the YAML config file.
/// @param config Output config (fields not present in the
///   file keep their defaults).
/// @returns void on success, or ConfigError.
auto LoadConfig(const char* path, ServerConfig* config)
    -> std::expected<void, Error<ConfigError>>;

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_CONFIG_H_
