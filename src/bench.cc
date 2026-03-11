/// @file bench.cc
/// @brief Benchmark instrumentation implementation.

#include "hyper_derp/bench.h"

#include <algorithm>
#include <cstring>
#include <print>

namespace hyper_derp {

void NowIso8601(char* buf, int buf_size) {
  time_t now = time(nullptr);
  struct tm tm;
  gmtime_r(&now, &tm);
  strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void LatencyRecorder::Init(int max_samples) {
  samples = std::make_unique<uint64_t[]>(max_samples);
  count = 0;
  capacity = max_samples;
}

void LatencyRecorder::Record(uint64_t latency_ns) {
  if (count < capacity) {
    samples[count++] = latency_ns;
  }
}

void LatencyRecorder::Sort() {
  std::sort(samples.get(), samples.get() + count);
}

uint64_t LatencyRecorder::Percentile(double p) const {
  if (count == 0) {
    return 0;
  }
  int idx = static_cast<int>(p * (count - 1));
  if (idx >= count) {
    idx = count - 1;
  }
  return samples[idx];
}

uint64_t LatencyRecorder::Min() const {
  if (count == 0) {
    return 0;
  }
  return *std::min_element(
      samples.get(), samples.get() + count);
}

uint64_t LatencyRecorder::Max() const {
  if (count == 0) {
    return 0;
  }
  return *std::max_element(
      samples.get(), samples.get() + count);
}

uint64_t LatencyRecorder::Mean() const {
  if (count == 0) {
    return 0;
  }
  uint64_t sum = 0;
  for (int i = 0; i < count; i++) {
    sum += samples[i];
  }
  return sum / static_cast<uint64_t>(count);
}

void WriteBenchJson(FILE* out, const BenchResult* r) {
  char ts[64];
  NowIso8601(ts, sizeof(ts));

  double elapsed_s = r->elapsed_ns / 1e9;
  double throughput_pps =
      (elapsed_s > 0)
          ? r->packets_completed / elapsed_s
          : 0;
  double throughput_mbps =
      (elapsed_s > 0)
          ? (r->bytes_total * 8.0) / (elapsed_s * 1e6)
          : 0;

  std::print(out, "{{\n");
  std::print(out, "  \"test\": \"{}\",\n", r->test_name);
  std::print(out, "  \"mode\": \"{}\",\n", r->mode);
  std::print(out, "  \"timestamp\": \"{}\",\n", ts);

  std::print(out, "  \"config\": {{\n");
  std::print(out, "    \"packet_count\": {},\n",
             r->packet_count);
  std::print(out, "    \"packet_size\": {},\n",
             r->packet_size);
  std::print(out, "    \"num_workers\": {},\n",
             r->num_workers);
  std::print(out, "    \"core\": {}\n", r->core);
  std::print(out, "  }},\n");

  std::print(out, "  \"elapsed_ns\": {},\n",
             r->elapsed_ns);
  std::print(out, "  \"elapsed_ms\": {:.3f},\n",
             r->elapsed_ns / 1e6);
  std::print(out, "  \"packets\": {},\n",
             r->packets_completed);
  std::print(out, "  \"bytes\": {},\n", r->bytes_total);
  std::print(out, "  \"throughput_pps\": {:.1f},\n",
             throughput_pps);
  std::print(out, "  \"throughput_mbps\": {:.3f}",
             throughput_mbps);

  if (r->latency && r->latency->count > 0) {
    std::print(out, ",\n");
    std::print(out, "  \"latency_ns\": {{\n");
    std::print(out, "    \"samples\": {},\n",
               r->latency->count);
    std::print(out, "    \"min\": {},\n",
               r->latency->Min());
    std::print(out, "    \"max\": {},\n",
               r->latency->Max());
    std::print(out, "    \"mean\": {},\n",
               r->latency->Mean());
    std::print(out, "    \"p50\": {},\n",
               r->latency->Percentile(0.50));
    std::print(out, "    \"p90\": {},\n",
               r->latency->Percentile(0.90));
    std::print(out, "    \"p99\": {},\n",
               r->latency->Percentile(0.99));
    std::print(out, "    \"p999\": {}",
               r->latency->Percentile(0.999));

    if (r->include_raw_samples) {
      std::print(out, ",\n    \"raw\": [");
      for (int i = 0; i < r->latency->count; i++) {
        if (i > 0) {
          std::print(out, ",");
        }
        if (i % 20 == 0) {
          std::print(out, "\n      ");
        }
        std::print(out, "{}", r->latency->samples[i]);
      }
      std::print(out, "\n    ]");
    }

    std::print(out, "\n  }}\n");
  } else {
    std::print(out, "\n");
  }

  std::print(out, "}}\n");
}

}  // namespace hyper_derp
