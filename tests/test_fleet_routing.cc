/// @file test_fleet_routing.cc
/// @brief Unit tests for fleet relay-to-relay routing.

#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/hd_relay_table.h"
#include "hyper_derp/types.h"

#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

// -- RelayTable tests ----------------------------------------

TEST(RelayTableTest, InitSetsFields) {
  RelayTable rt;
  RelayTableInit(&rt, 42);
  EXPECT_EQ(rt.self_id, 42);
  EXPECT_EQ(rt.relay_count, 0);
}

TEST(RelayTableTest, AddAndLookup) {
  RelayTable rt;
  RelayTableInit(&rt, 1);

  Key key{};
  std::memset(key.data(), 0xAA, kKeySize);
  auto* entry =
      RelayTableAdd(&rt, 5, 100, key, "relay-5");

  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->relay_id, 5);
  EXPECT_EQ(entry->fd, 100);
  EXPECT_EQ(entry->key, key);
  EXPECT_EQ(entry->hops, 1);
  EXPECT_EQ(entry->next_hop, 5);
  EXPECT_STREQ(entry->name, "relay-5");
  EXPECT_EQ(entry->occupied, 1);
  EXPECT_EQ(rt.relay_count, 1);

  auto* found = RelayTableLookup(&rt, 5);
  EXPECT_EQ(found, entry);

  auto* missing = RelayTableLookup(&rt, 99);
  EXPECT_EQ(missing, nullptr);
}

TEST(RelayTableTest, AddZeroIdReturnsNull) {
  RelayTable rt;
  RelayTableInit(&rt, 1);
  Key key{};
  auto* entry =
      RelayTableAdd(&rt, 0, 10, key, "bad");
  EXPECT_EQ(entry, nullptr);
  EXPECT_EQ(rt.relay_count, 0);
}

TEST(RelayTableTest, AddDuplicateUpdates) {
  RelayTable rt;
  RelayTableInit(&rt, 1);

  Key key1{};
  std::memset(key1.data(), 0x11, kKeySize);
  RelayTableAdd(&rt, 5, 100, key1, "old");
  EXPECT_EQ(rt.relay_count, 1);

  Key key2{};
  std::memset(key2.data(), 0x22, kKeySize);
  auto* entry =
      RelayTableAdd(&rt, 5, 200, key2, "new");

  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->fd, 200);
  EXPECT_EQ(entry->key, key2);
  EXPECT_STREQ(entry->name, "new");
  // Count unchanged on update.
  EXPECT_EQ(rt.relay_count, 1);
}

TEST(RelayTableTest, Remove) {
  RelayTable rt;
  RelayTableInit(&rt, 1);

  Key key{};
  RelayTableAdd(&rt, 5, 10, key, "r5");
  EXPECT_EQ(rt.relay_count, 1);

  RelayTableRemove(&rt, 5);
  EXPECT_EQ(rt.relay_count, 0);
  EXPECT_EQ(RelayTableLookup(&rt, 5), nullptr);

  // Removing non-existent entry is a no-op.
  RelayTableRemove(&rt, 99);
  EXPECT_EQ(rt.relay_count, 0);
}

TEST(RelayTableTest, UpdateRouteImproves) {
  RelayTable rt;
  RelayTableInit(&rt, 1);

  Key key{};
  RelayTableAdd(&rt, 5, 100, key, "r5");
  auto* entry = RelayTableLookup(&rt, 5);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->hops, 1);

  // Higher hop count should not replace.
  RelayTableUpdateRoute(&rt, 5, 3, 10, 200);
  entry = RelayTableLookup(&rt, 5);
  EXPECT_EQ(entry->hops, 1);
  EXPECT_EQ(entry->fd, 100);
}

TEST(RelayTableTest, UpdateRouteCreatesNew) {
  RelayTable rt;
  RelayTableInit(&rt, 1);

  // Route announcement for an unknown relay.
  RelayTableUpdateRoute(&rt, 10, 2, 5, 100);
  EXPECT_EQ(rt.relay_count, 1);

  auto* entry = RelayTableLookup(&rt, 10);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->hops, 2);
  EXPECT_EQ(entry->next_hop, 5);
  EXPECT_EQ(entry->fd, 100);
}

TEST(RelayTableTest, UpdateRouteIgnoresSelf) {
  RelayTable rt;
  RelayTableInit(&rt, 7);
  // Route to self should be ignored.
  RelayTableUpdateRoute(&rt, 7, 0, 7, 100);
  EXPECT_EQ(rt.relay_count, 0);
}

TEST(RelayTableTest, UpdateRouteIgnoresZero) {
  RelayTable rt;
  RelayTableInit(&rt, 1);
  RelayTableUpdateRoute(&rt, 0, 1, 5, 100);
  EXPECT_EQ(rt.relay_count, 0);
}

TEST(RelayTableTest, ListRelays) {
  RelayTable rt;
  RelayTableInit(&rt, 1);

  Key key{};
  RelayTableAdd(&rt, 3, 10, key, "r3");
  RelayTableAdd(&rt, 7, 20, key, "r7");
  RelayTableAdd(&rt, 11, 30, key, "r11");

  uint16_t ids[10];
  uint8_t hops[10];
  int count = RelayTableList(&rt, ids, hops, 10);
  EXPECT_EQ(count, 3);

  // Verify all IDs are present (order not guaranteed).
  bool found3 = false, found7 = false, found11 = false;
  for (int i = 0; i < count; i++) {
    if (ids[i] == 3) found3 = true;
    if (ids[i] == 7) found7 = true;
    if (ids[i] == 11) found11 = true;
  }
  EXPECT_TRUE(found3);
  EXPECT_TRUE(found7);
  EXPECT_TRUE(found11);
}

TEST(RelayTableTest, ListMaxOut) {
  RelayTable rt;
  RelayTableInit(&rt, 1);

  Key key{};
  for (int i = 1; i <= 5; i++) {
    RelayTableAdd(
        &rt, static_cast<uint16_t>(i), i * 10, key,
        "r");
  }

  uint16_t ids[3];
  uint8_t hops[3];
  int count = RelayTableList(&rt, ids, hops, 3);
  EXPECT_EQ(count, 3);
}

TEST(RelayTableTest, TableFull) {
  RelayTable rt;
  RelayTableInit(&rt, 1);

  Key key{};
  // Fill all slots.
  for (int i = 0; i < kMaxRelays; i++) {
    auto* e = RelayTableAdd(
        &rt, static_cast<uint16_t>(i + 1), i, key,
        nullptr);
    ASSERT_NE(e, nullptr)
        << "Failed to add relay " << (i + 1);
  }
  EXPECT_EQ(rt.relay_count, kMaxRelays);

  // One more should fail.
  auto* over = RelayTableAdd(
      &rt, static_cast<uint16_t>(kMaxRelays + 1), 999,
      key, nullptr);
  EXPECT_EQ(over, nullptr);
}

// -- RouteAnnounce codec tests -------------------------------

TEST(RouteAnnounceTest, BuildAndParseRoundtrip) {
  uint16_t ids[] = {1, 5, 100};
  uint8_t hops[] = {0, 1, 2};
  int count = 3;

  uint8_t buf[256];
  int n = HdBuildRouteAnnounce(buf, sizeof(buf),
                               ids, hops, count);

  ASSERT_GT(n, 0);
  EXPECT_EQ(n, kHdFrameHeaderSize +
               count * kHdRouteEntrySize);

  // Verify frame header.
  EXPECT_EQ(HdReadFrameType(buf),
            HdFrameType::kRouteAnnounce);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(
                count * kHdRouteEntrySize));

  // Parse payload.
  const uint8_t* payload = buf + kHdFrameHeaderSize;
  int payload_len = n - kHdFrameHeaderSize;
  uint16_t out_ids[10];
  uint8_t out_hops[10];
  int parsed = HdParseRouteAnnounce(
      payload, payload_len, out_ids, out_hops, 10);

  EXPECT_EQ(parsed, count);
  for (int i = 0; i < count; i++) {
    EXPECT_EQ(out_ids[i], ids[i]);
    EXPECT_EQ(out_hops[i], hops[i]);
  }
}

TEST(RouteAnnounceTest, EmptyAnnouncement) {
  uint8_t buf[16];
  int n = HdBuildRouteAnnounce(
      buf, sizeof(buf), nullptr, nullptr, 0);
  ASSERT_GT(n, 0);
  EXPECT_EQ(n, kHdFrameHeaderSize);

  uint16_t out_ids[1];
  uint8_t out_hops[1];
  int parsed = HdParseRouteAnnounce(
      buf + kHdFrameHeaderSize, 0,
      out_ids, out_hops, 1);
  EXPECT_EQ(parsed, 0);
}

TEST(RouteAnnounceTest, BufferTooSmall) {
  uint16_t ids[] = {1, 2, 3};
  uint8_t hops[] = {1, 1, 1};
  uint8_t buf[4];  // Way too small.
  int n = HdBuildRouteAnnounce(
      buf, sizeof(buf), ids, hops, 3);
  EXPECT_EQ(n, -1);
}

TEST(RouteAnnounceTest, ParseMaxOut) {
  uint16_t ids[] = {1, 2, 3, 4, 5};
  uint8_t hops[] = {1, 1, 2, 3, 4};
  uint8_t buf[256];
  int n = HdBuildRouteAnnounce(
      buf, sizeof(buf), ids, hops, 5);
  ASSERT_GT(n, 0);

  uint16_t out_ids[2];
  uint8_t out_hops[2];
  int parsed = HdParseRouteAnnounce(
      buf + kHdFrameHeaderSize,
      n - kHdFrameHeaderSize,
      out_ids, out_hops, 2);
  EXPECT_EQ(parsed, 2);
  EXPECT_EQ(out_ids[0], 1);
  EXPECT_EQ(out_ids[1], 2);
}

TEST(RouteAnnounceTest, PartialEntry) {
  // Payload with 3 full entries + 1 extra byte.
  uint16_t ids[] = {1, 2, 3};
  uint8_t hops[] = {0, 1, 2};
  uint8_t buf[256];
  int n = HdBuildRouteAnnounce(
      buf, sizeof(buf), ids, hops, 3);
  ASSERT_GT(n, 0);

  // Parse with payload_len that has a partial entry.
  int payload_len = 3 * kHdRouteEntrySize + 1;
  uint16_t out_ids[10];
  uint8_t out_hops[10];
  int parsed = HdParseRouteAnnounce(
      buf + kHdFrameHeaderSize, payload_len,
      out_ids, out_hops, 10);
  // Partial entry should be discarded.
  EXPECT_EQ(parsed, 3);
}

// -- FleetData local delivery test ---------------------------

TEST(FleetDataTest, LocalDeliveryFrameLayout) {
  // Build a FleetData frame and verify the local
  // delivery logic would strip the 4B fleet header
  // correctly.
  uint8_t buf[256];
  uint16_t relay_id = 1;
  uint16_t peer_id = 42;
  int payload = 100;
  int n = HdBuildFleetDataHeader(
      buf, relay_id, peer_id, payload);
  EXPECT_EQ(n, kHdFrameHeaderSize + kHdFleetDstSize);

  // Fill mock payload.
  for (int i = 0; i < payload; i++) {
    buf[n + i] = static_cast<uint8_t>(i & 0xFF);
  }

  // Read back the fleet header fields.
  const uint8_t* fleet_payload =
      buf + kHdFrameHeaderSize;
  EXPECT_EQ(HdReadFleetRelay(fleet_payload), relay_id);
  EXPECT_EQ(HdReadFleetPeer(fleet_payload), peer_id);

  // The inner payload starts after the 4B fleet dst.
  const uint8_t* inner =
      fleet_payload + kHdFleetDstSize;
  EXPECT_EQ(inner[0], 0);
  EXPECT_EQ(inner[1], 1);
  EXPECT_EQ(inner[99], 99);
}

TEST(FleetDataTest, RemoteForwardFramePreserved) {
  // Verify that a FleetData frame for a remote relay
  // should be forwarded as-is (no header rewrite).
  uint8_t buf[256];
  uint16_t dst_relay = 5;
  uint16_t dst_peer = 99;
  int payload = 50;
  int total = HdBuildFleetDataHeader(
      buf, dst_relay, dst_peer, payload);
  for (int i = 0; i < payload; i++) {
    buf[total + i] = static_cast<uint8_t>(i);
  }
  int frame_len = total + payload;

  // Simulate a copy (as the data plane would do).
  uint8_t copy[256];
  std::memcpy(copy, buf, frame_len);

  // Verify the copy is identical.
  EXPECT_EQ(std::memcmp(buf, copy, frame_len), 0);
  EXPECT_EQ(HdReadFrameType(copy),
            HdFrameType::kFleetData);
  auto* cpay = copy + kHdFrameHeaderSize;
  EXPECT_EQ(HdReadFleetRelay(cpay), dst_relay);
  EXPECT_EQ(HdReadFleetPeer(cpay), dst_peer);
}

// -- Ctx relay_peer_map tests --------------------------------

TEST(CtxRelayMapTest, DefaultZero) {
  Ctx ctx{};
  for (int i = 0; i < 256; i++) {
    EXPECT_EQ(ctx.relay_peer_map[i], 0);
  }
  EXPECT_EQ(ctx.relay_id, 0);
}

TEST(CtxRelayMapTest, SetAndRead) {
  Ctx ctx{};
  ctx.relay_id = 1;
  ctx.relay_peer_map[5] = 42;
  EXPECT_EQ(ctx.relay_peer_map[5], 42);
  EXPECT_EQ(ctx.relay_peer_map[6], 0);
}

}  // namespace hyper_derp
