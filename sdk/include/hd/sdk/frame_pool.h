/// @file frame_pool.h
/// @brief Zero-copy frame buffer pool.

#ifndef HD_SDK_FRAME_POOL_H_
#define HD_SDK_FRAME_POOL_H_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace hd::sdk {

/// Maximum frame payload size (HD protocol limit).
inline constexpr int kMaxFrameSize = 65536;

/// A buffer from the frame pool. Returned to the pool
/// on destruction (RAII).
class FrameBuffer {
 public:
  ~FrameBuffer();
  FrameBuffer(FrameBuffer&& o) noexcept;
  FrameBuffer& operator=(FrameBuffer&& o) noexcept;
  FrameBuffer(const FrameBuffer&) = delete;
  FrameBuffer& operator=(const FrameBuffer&) = delete;

  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }
  int size() const { return size_; }
  int capacity() const { return capacity_; }
  void set_size(int s) { size_ = s; }

 private:
  friend class FramePool;
  FrameBuffer(uint8_t* data, int capacity,
              std::atomic<int>* free_head, int* next,
              int slot);
  uint8_t* data_ = nullptr;
  int size_ = 0;
  int capacity_ = 0;
  std::atomic<int>* free_head_ = nullptr;
  int* next_ = nullptr;
  int slot_ = -1;
};

/// Lock-free frame pool. Fixed size, preallocated.
class FramePool {
 public:
  explicit FramePool(int count = 4096,
                     int buf_size = kMaxFrameSize);
  ~FramePool();

  FramePool(const FramePool&) = delete;
  FramePool& operator=(const FramePool&) = delete;

  /// Allocate a buffer. Returns nullptr if exhausted.
  std::unique_ptr<FrameBuffer> Alloc();

  /// Number of free buffers.
  int FreeCount() const;

 private:
  friend class FrameBuffer;
  int count_;
  int buf_size_;
  uint8_t* storage_ = nullptr;  // count_ * buf_size_
  int* next_ = nullptr;         // Freelist links.
  std::atomic<int> free_head_{0};
};

}  // namespace hd::sdk

#endif  // HD_SDK_FRAME_POOL_H_
