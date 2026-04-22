/// @file test_einheit_protocol.cc
/// @brief Round-trip tests for the einheit envelope codec. Hand-
///   crafted byte vectors match the encoding emitted by the
///   einheit-cli framework so drift on either side gets caught.

#include "hyper_derp/einheit_protocol.h"

#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace hyper_derp::einheit {
namespace {

TEST(EinheitProtocolTest, RequestRoundtrip) {
  Request req;
  req.id = "abc-123";
  req.user = "alice";
  req.role = "admin";
  req.session_id = "sess-1";
  req.command = "show_peers";
  req.args = {"ck_abc", "ck_def"};

  auto encoded = EncodeRequest(req);
  ASSERT_TRUE(encoded.has_value());

  auto decoded = DecodeRequest(*encoded);
  ASSERT_TRUE(decoded.has_value())
      << decoded.error().message;
  EXPECT_EQ(decoded->id, req.id);
  EXPECT_EQ(decoded->user, req.user);
  EXPECT_EQ(decoded->role, req.role);
  ASSERT_TRUE(decoded->session_id.has_value());
  EXPECT_EQ(*decoded->session_id, *req.session_id);
  EXPECT_EQ(decoded->command, req.command);
  EXPECT_EQ(decoded->args, req.args);
}

TEST(EinheitProtocolTest, RequestWithoutSession) {
  Request req;
  req.id = "no-session";
  req.command = "show_status";
  auto encoded = EncodeRequest(req);
  ASSERT_TRUE(encoded.has_value());
  auto decoded = DecodeRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_FALSE(decoded->session_id.has_value());
}

TEST(EinheitProtocolTest, ResponseRoundtrip) {
  Response res;
  res.id = "abc-123";
  res.status = ResponseStatus::kOk;
  res.data = {0x81, 0xa4, 't', 'y', 'p', 'e',
              0xa5, 'p', 'e', 'e', 'r', 's'};

  auto encoded = EncodeResponse(res);
  ASSERT_TRUE(encoded.has_value());

  auto decoded = DecodeResponse(*encoded);
  ASSERT_TRUE(decoded.has_value())
      << decoded.error().message;
  EXPECT_EQ(decoded->id, res.id);
  EXPECT_EQ(decoded->status, ResponseStatus::kOk);
  EXPECT_EQ(decoded->data, res.data);
  EXPECT_FALSE(decoded->error.has_value());
}

TEST(EinheitProtocolTest, ResponseErrorRoundtrip) {
  Response res;
  res.id = "fail";
  res.status = ResponseStatus::kError;
  res.error = ResponseError{"not_found",
                             "peer not found",
                             "check the key"};

  auto encoded = EncodeResponse(res);
  ASSERT_TRUE(encoded.has_value());

  auto decoded = DecodeResponse(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status, ResponseStatus::kError);
  ASSERT_TRUE(decoded->error.has_value());
  EXPECT_EQ(decoded->error->code, "not_found");
  EXPECT_EQ(decoded->error->message, "peer not found");
  EXPECT_EQ(decoded->error->hint, "check the key");
}

TEST(EinheitProtocolTest, EventBodyRoundtrip) {
  Event ev;
  ev.topic = "state.peers.ck_abc";
  ev.timestamp = "2026-04-23T12:00:00Z";
  ev.data = {1, 2, 3, 4};

  auto encoded = EncodeEventBody(ev);
  ASSERT_TRUE(encoded.has_value());

  auto decoded = DecodeEventBody(ev.topic, *encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->topic, ev.topic);
  EXPECT_EQ(decoded->timestamp, ev.timestamp);
  EXPECT_EQ(decoded->data, ev.data);
}

TEST(EinheitProtocolTest, RejectsVersionMismatch) {
  Request req;
  req.envelope_version = 99;
  req.command = "show_status";
  auto encoded = EncodeRequest(req);
  ASSERT_TRUE(encoded.has_value());
  auto decoded = DecodeRequest(*encoded);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().code,
            CodecError::kVersionMismatch);
}

TEST(EinheitProtocolTest, RejectsNonMap) {
  std::vector<std::uint8_t> not_map = {0xa3, 'f', 'o', 'o'};
  auto r = DecodeRequest(not_map);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, CodecError::kTypeMismatch);
}

TEST(EinheitProtocolTest, RejectsTruncatedInput) {
  std::vector<std::uint8_t> truncated = {0x81, 0xa2, 'i'};
  auto r = DecodeRequest(truncated);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, CodecError::kExceptionRaised);
}

// -- Cross-compat vectors --------------------------------
//
// The byte sequences below were captured from einheit-cli's
// EncodeRequest / EncodeResponse for a fixed input. They
// serve as a byte-level canary: if our encoder diverges, the
// decode tests still pass but these break — forcing the
// drift to be resolved at the encoder, not silently on one
// side of the wire.

TEST(EinheitProtocolTest, DecodesFrameworkRequestVector) {
  const std::vector<std::uint8_t> framework_bytes = {
      0x87, 0xb0, 0x65, 0x6e, 0x76, 0x65, 0x6c, 0x6f,
      0x70, 0x65, 0x5f, 0x76, 0x65, 0x72, 0x73, 0x69,
      0x6f, 0x6e, 0x01, 0xa2, 0x69, 0x64, 0xa5, 0x76,
      0x65, 0x63, 0x2d, 0x31, 0xa4, 0x75, 0x73, 0x65,
      0x72, 0xa5, 0x61, 0x6c, 0x69, 0x63, 0x65, 0xa4,
      0x72, 0x6f, 0x6c, 0x65, 0xa5, 0x61, 0x64, 0x6d,
      0x69, 0x6e, 0xaa, 0x73, 0x65, 0x73, 0x73, 0x69,
      0x6f, 0x6e, 0x5f, 0x69, 0x64, 0xc0, 0xa7, 0x63,
      0x6f, 0x6d, 0x6d, 0x61, 0x6e, 0x64, 0xaa, 0x73,
      0x68, 0x6f, 0x77, 0x5f, 0x70, 0x65, 0x65, 0x72,
      0x73, 0xa4, 0x61, 0x72, 0x67, 0x73, 0x91, 0xa6,
      0x63, 0x6b, 0x5f, 0x61, 0x62, 0x63};
  auto r = DecodeRequest(framework_bytes);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->id, "vec-1");
  EXPECT_EQ(r->user, "alice");
  EXPECT_EQ(r->role, "admin");
  EXPECT_EQ(r->command, "show_peers");
  EXPECT_EQ(r->args, std::vector<std::string>{"ck_abc"});
  EXPECT_FALSE(r->session_id.has_value());

  // Round-trip: our encoder must re-emit the same bytes the
  // framework produced.
  auto encoded = EncodeRequest(*r);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(*encoded, framework_bytes);
}

TEST(EinheitProtocolTest, DecodesFrameworkResponseVector) {
  const std::vector<std::uint8_t> framework_bytes = {
      0x85, 0xb0, 0x65, 0x6e, 0x76, 0x65, 0x6c, 0x6f,
      0x70, 0x65, 0x5f, 0x76, 0x65, 0x72, 0x73, 0x69,
      0x6f, 0x6e, 0x01, 0xa2, 0x69, 0x64, 0xa5, 0x76,
      0x65, 0x63, 0x2d, 0x31, 0xa6, 0x73, 0x74, 0x61,
      0x74, 0x75, 0x73, 0xa2, 0x6f, 0x6b, 0xa4, 0x64,
      0x61, 0x74, 0x61, 0xc4, 0x02, 0xde, 0xad, 0xa5,
      0x65, 0x72, 0x72, 0x6f, 0x72, 0xc0};
  auto r = DecodeResponse(framework_bytes);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->id, "vec-1");
  EXPECT_EQ(r->status, ResponseStatus::kOk);
  EXPECT_EQ(r->data,
            (std::vector<std::uint8_t>{0xde, 0xad}));
  EXPECT_FALSE(r->error.has_value());

  auto encoded = EncodeResponse(*r);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(*encoded, framework_bytes);
}

}  // namespace
}  // namespace hyper_derp::einheit
