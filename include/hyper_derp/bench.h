/// @file bench.h
/// @brief Benchmark instrumentation: high-resolution timing,
///   latency recording, and JSON result serialization.

#ifndef INCLUDE_HYPER_DERP_BENCH_H_
#define INCLUDE_HYPER_DERP_BENCH_H_

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>

namespace hyper_derp {

/// @brief Get current time in nanoseconds (CLOCK_MONOTONIC).
/// @returns Monotonic nanosecond timestamp.
inline uint64_t NowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

/// @brief Write current time as ISO 8601 string.
/// @param buf Output buffer.
/// @param buf_size Buffer capacity (min 32).
void NowIso8601(char* buf, int buf_size);

/// @brief Per-sample latency recorder with percentile
///   computation.
struct LatencyRecorder {
  std::unique_ptr<uint64_t[]> samples;
  int count = 0;
  int capacity = 0;

  LatencyRecorder() = default;
  ~LatencyRecorder() = default;

  // Movable, not copyable.
  LatencyRecorder(LatencyRecorder&&) = default;
  LatencyRecorder& operator=(LatencyRecorder&&) = default;
  LatencyRecorder(const LatencyRecorder&) = delete;
  LatencyRecorder& operator=(const LatencyRecorder&) = delete;

  /// @brief Allocate storage for up to max_samples.
  void Init(int max_samples);

  /// @brief Record a single latency sample.
  /// @param latency_ns Latency in nanoseconds.
  void Record(uint64_t latency_ns);

  /// @brief Sort samples (required before Percentile).
  void Sort();

  /// @brief Compute a percentile value.
  /// @param p Percentile in [0.0, 1.0].
  /// @returns Latency at the given percentile.
  uint64_t Percentile(double p) const;

  /// @returns Minimum sample value.
  uint64_t Min() const;

  /// @returns Maximum sample value.
  uint64_t Max() const;

  /// @returns Arithmetic mean of all samples.
  uint64_t Mean() const;
};

/// @brief Benchmark result for JSON serialization.
struct BenchResult {
  const char* test_name;
  const char* mode;
  int packet_count;
  int packet_size;
  int num_workers;
  int core;
  uint64_t elapsed_ns;
  int packets_completed;
  uint64_t bytes_total;
  const LatencyRecorder* latency;
  bool include_raw_samples;
};

/// @brief Write benchmark results as JSON.
/// @param out Output file (stdout, or opened file).
/// @param r Result data to serialize.
void WriteBenchJson(FILE* out, const BenchResult* r);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_BENCH_H_
