/// @file test_bench.cc
/// @brief Unit tests for benchmark instrumentation.

#include "hyper_derp/bench.h"

#include <cstdio>
#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

TEST(BenchTest, NowNsMonotonic) {
  uint64_t t0 = NowNs();
  uint64_t t1 = NowNs();
  EXPECT_GE(t1, t0);
}

TEST(BenchTest, NowIso8601Format) {
  char buf[64];
  NowIso8601(buf, sizeof(buf));
  // Should be like "2026-03-10T12:00:00Z"
  EXPECT_EQ(strlen(buf), 20u);
  EXPECT_EQ(buf[4], '-');
  EXPECT_EQ(buf[7], '-');
  EXPECT_EQ(buf[10], 'T');
  EXPECT_EQ(buf[19], 'Z');
}

TEST(BenchTest, LatencyRecorderBasic) {
  LatencyRecorder lr;
  lr.Init(100);

  lr.Record(100);
  lr.Record(200);
  lr.Record(300);
  lr.Record(400);
  lr.Record(500);

  EXPECT_EQ(lr.count, 5);
  EXPECT_EQ(lr.Min(), 100u);
  EXPECT_EQ(lr.Max(), 500u);
  EXPECT_EQ(lr.Mean(), 300u);

  lr.Sort();
  EXPECT_EQ(lr.Percentile(0.0), 100u);
  EXPECT_EQ(lr.Percentile(0.5), 300u);
  EXPECT_EQ(lr.Percentile(1.0), 500u);
}

TEST(BenchTest, LatencyRecorderCapacity) {
  LatencyRecorder lr;
  lr.Init(3);

  lr.Record(10);
  lr.Record(20);
  lr.Record(30);
  lr.Record(40);

  EXPECT_EQ(lr.count, 3);
}

TEST(BenchTest, LatencyRecorderEmpty) {
  LatencyRecorder lr;
  lr.Init(10);

  EXPECT_EQ(lr.Min(), 0u);
  EXPECT_EQ(lr.Max(), 0u);
  EXPECT_EQ(lr.Mean(), 0u);
  EXPECT_EQ(lr.Percentile(0.5), 0u);
}

TEST(BenchTest, WriteBenchJsonThroughput) {
  BenchResult r = {};
  r.test_name = "throughput_test";
  r.mode = "send";
  r.packet_count = 1000;
  r.packet_size = 128;
  r.num_workers = 1;
  r.core = -1;
  r.elapsed_ns = 1000000000ULL;
  r.packets_completed = 1000;
  r.bytes_total = 128000;
  r.latency = nullptr;
  r.include_raw_samples = false;

  char buf[4096];
  FILE* f = fmemopen(buf, sizeof(buf), "w");
  ASSERT_NE(f, nullptr);
  WriteBenchJson(f, &r);
  fclose(f);

  EXPECT_TRUE(strstr(buf, "\"throughput_test\""));
  EXPECT_TRUE(strstr(buf, "\"send\""));
  EXPECT_TRUE(strstr(buf, "\"throughput_pps\""));
  EXPECT_TRUE(strstr(buf, "\"throughput_mbps\""));
  EXPECT_TRUE(strstr(buf, "\"elapsed_ms\""));
  EXPECT_FALSE(strstr(buf, "\"latency_ns\""));
}

TEST(BenchTest, WriteBenchJsonWithLatency) {
  LatencyRecorder lr;
  lr.Init(5);
  lr.Record(10000);
  lr.Record(20000);
  lr.Record(30000);
  lr.Record(40000);
  lr.Record(50000);
  lr.Sort();

  BenchResult r = {};
  r.test_name = "latency_test";
  r.mode = "ping";
  r.packet_count = 5;
  r.packet_size = 64;
  r.num_workers = 1;
  r.core = 2;
  r.elapsed_ns = 500000000ULL;
  r.packets_completed = 5;
  r.bytes_total = 640;
  r.latency = &lr;
  r.include_raw_samples = false;

  char buf[4096];
  FILE* f = fmemopen(buf, sizeof(buf), "w");
  ASSERT_NE(f, nullptr);
  WriteBenchJson(f, &r);
  fclose(f);

  EXPECT_TRUE(strstr(buf, "\"latency_ns\""));
  EXPECT_TRUE(strstr(buf, "\"p50\""));
  EXPECT_TRUE(strstr(buf, "\"p99\""));
  EXPECT_TRUE(strstr(buf, "\"min\""));
  EXPECT_TRUE(strstr(buf, "\"max\""));
  EXPECT_FALSE(strstr(buf, "\"raw\""));
}

TEST(BenchTest, WriteBenchJsonRawSamples) {
  LatencyRecorder lr;
  lr.Init(3);
  lr.Record(1000);
  lr.Record(2000);
  lr.Record(3000);
  lr.Sort();

  BenchResult r = {};
  r.test_name = "raw_test";
  r.mode = "ping";
  r.packet_count = 3;
  r.packet_size = 64;
  r.num_workers = 1;
  r.core = -1;
  r.elapsed_ns = 100000000ULL;
  r.packets_completed = 3;
  r.bytes_total = 384;
  r.latency = &lr;
  r.include_raw_samples = true;

  char buf[4096];
  FILE* f = fmemopen(buf, sizeof(buf), "w");
  ASSERT_NE(f, nullptr);
  WriteBenchJson(f, &r);
  fclose(f);

  EXPECT_TRUE(strstr(buf, "\"raw\""));
  EXPECT_TRUE(strstr(buf, "1000"));
  EXPECT_TRUE(strstr(buf, "2000"));
  EXPECT_TRUE(strstr(buf, "3000"));
}

}  // namespace hyper_derp
