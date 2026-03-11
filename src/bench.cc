/// @file bench.cc
/// @brief Benchmark instrumentation implementation.

#include "hyper_derp/bench.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

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

  fprintf(out, "{\n");
  fprintf(out, "  \"test\": \"%s\",\n", r->test_name);
  fprintf(out, "  \"mode\": \"%s\",\n", r->mode);
  fprintf(out, "  \"timestamp\": \"%s\",\n", ts);

  fprintf(out, "  \"config\": {\n");
  fprintf(out, "    \"packet_count\": %d,\n",
          r->packet_count);
  fprintf(out, "    \"packet_size\": %d,\n",
          r->packet_size);
  fprintf(out, "    \"num_workers\": %d,\n",
          r->num_workers);
  fprintf(out, "    \"core\": %d\n", r->core);
  fprintf(out, "  },\n");

  fprintf(out, "  \"elapsed_ns\": %" PRIu64 ",\n",
          r->elapsed_ns);
  fprintf(out, "  \"elapsed_ms\": %.3f,\n",
          r->elapsed_ns / 1e6);
  fprintf(out, "  \"packets\": %d,\n",
          r->packets_completed);
  fprintf(out, "  \"bytes\": %" PRIu64 ",\n",
          r->bytes_total);
  fprintf(out, "  \"throughput_pps\": %.1f,\n",
          throughput_pps);
  fprintf(out, "  \"throughput_mbps\": %.3f",
          throughput_mbps);

  if (r->latency && r->latency->count > 0) {
    fprintf(out, ",\n");
    fprintf(out, "  \"latency_ns\": {\n");
    fprintf(out, "    \"samples\": %d,\n",
            r->latency->count);
    fprintf(out, "    \"min\": %" PRIu64 ",\n",
            r->latency->Min());
    fprintf(out, "    \"max\": %" PRIu64 ",\n",
            r->latency->Max());
    fprintf(out, "    \"mean\": %" PRIu64 ",\n",
            r->latency->Mean());
    fprintf(out, "    \"p50\": %" PRIu64 ",\n",
            r->latency->Percentile(0.50));
    fprintf(out, "    \"p90\": %" PRIu64 ",\n",
            r->latency->Percentile(0.90));
    fprintf(out, "    \"p99\": %" PRIu64 ",\n",
            r->latency->Percentile(0.99));
    fprintf(out, "    \"p999\": %" PRIu64 "",
            r->latency->Percentile(0.999));

    if (r->include_raw_samples) {
      fprintf(out, ",\n    \"raw\": [");
      for (int i = 0; i < r->latency->count; i++) {
        if (i > 0) {
          fprintf(out, ",");
        }
        if (i % 20 == 0) {
          fprintf(out, "\n      ");
        }
        fprintf(out, "%" PRIu64,
                r->latency->samples[i]);
      }
      fprintf(out, "\n    ]");
    }

    fprintf(out, "\n  }\n");
  } else {
    fprintf(out, "\n");
  }

  fprintf(out, "}\n");
}

}  // namespace hyper_derp
