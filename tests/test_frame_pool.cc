/// @file test_frame_pool.cc
/// @brief Tests for the zero-copy frame pool.

#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include <hd/sdk/frame_pool.h>

namespace hd::sdk {
namespace {

TEST(FramePoolTest, AllocAndReturn) {
  FramePool pool(4, 1024);
  EXPECT_EQ(pool.FreeCount(), 4);

  auto b1 = pool.Alloc();
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ(b1->capacity(), 1024);
  EXPECT_EQ(b1->size(), 0);
  EXPECT_EQ(pool.FreeCount(), 3);

  auto b2 = pool.Alloc();
  ASSERT_NE(b2, nullptr);
  EXPECT_EQ(pool.FreeCount(), 2);

  // Return b1.
  b1.reset();
  EXPECT_EQ(pool.FreeCount(), 3);

  // Return b2.
  b2.reset();
  EXPECT_EQ(pool.FreeCount(), 4);
}

TEST(FramePoolTest, Exhaustion) {
  FramePool pool(2, 64);
  auto b1 = pool.Alloc();
  auto b2 = pool.Alloc();
  ASSERT_NE(b1, nullptr);
  ASSERT_NE(b2, nullptr);

  auto b3 = pool.Alloc();
  EXPECT_EQ(b3, nullptr);

  // Return one, alloc again.
  b1.reset();
  auto b4 = pool.Alloc();
  EXPECT_NE(b4, nullptr);
}

TEST(FramePoolTest, WriteAndRead) {
  FramePool pool(1, 256);
  auto buf = pool.Alloc();
  ASSERT_NE(buf, nullptr);

  const char* msg = "hello-pool";
  int len = static_cast<int>(strlen(msg));
  memcpy(buf->data(), msg, len);
  buf->set_size(len);

  EXPECT_EQ(buf->size(), len);
  EXPECT_EQ(memcmp(buf->data(), msg, len), 0);
}

TEST(FramePoolTest, MoveSemantics) {
  FramePool pool(2, 64);
  auto b1 = pool.Alloc();
  EXPECT_EQ(pool.FreeCount(), 1);

  // Move.
  auto b2 = std::move(b1);
  EXPECT_EQ(b1, nullptr);
  EXPECT_NE(b2, nullptr);
  EXPECT_EQ(pool.FreeCount(), 1);

  // Move-assign.
  auto b3 = pool.Alloc();
  EXPECT_EQ(pool.FreeCount(), 0);
  b3 = std::move(b2);  // b3's old slot returned.
  EXPECT_EQ(pool.FreeCount(), 1);
}

TEST(FramePoolTest, LargePool) {
  FramePool pool(4096, 1400);
  std::vector<std::unique_ptr<FrameBuffer>> bufs;
  for (int i = 0; i < 4096; i++) {
    auto b = pool.Alloc();
    ASSERT_NE(b, nullptr) << "exhausted at " << i;
    bufs.push_back(std::move(b));
  }
  EXPECT_EQ(pool.FreeCount(), 0);
  EXPECT_EQ(pool.Alloc(), nullptr);

  bufs.clear();
  EXPECT_EQ(pool.FreeCount(), 4096);
}

}  // namespace
}  // namespace hd::sdk
