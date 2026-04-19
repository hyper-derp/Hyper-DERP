/// @file frame_pool.cc
/// @brief Zero-copy frame buffer pool.

#include "hd/sdk/frame_pool.h"

#include <new>

namespace hd::sdk {

// -- FrameBuffer -------------------------------------------------------------

FrameBuffer::FrameBuffer(uint8_t* data, int capacity,
                         std::atomic<int>* free_head,
                         int* next, int slot)
    : data_(data), size_(0), capacity_(capacity),
      free_head_(free_head), next_(next), slot_(slot) {}

FrameBuffer::~FrameBuffer() {
  if (slot_ >= 0 && free_head_) {
    // Return to pool (lock-free push).
    int old_head = free_head_->load(
        std::memory_order_relaxed);
    do {
      next_[slot_] = old_head;
    } while (!free_head_->compare_exchange_weak(
        old_head, slot_,
        std::memory_order_release,
        std::memory_order_relaxed));
  }
}

FrameBuffer::FrameBuffer(FrameBuffer&& o) noexcept
    : data_(o.data_), size_(o.size_),
      capacity_(o.capacity_), free_head_(o.free_head_),
      next_(o.next_), slot_(o.slot_) {
  o.slot_ = -1;
  o.data_ = nullptr;
}

FrameBuffer& FrameBuffer::operator=(
    FrameBuffer&& o) noexcept {
  if (this != &o) {
    // Return current buffer if owned.
    if (slot_ >= 0 && free_head_) {
      int old = free_head_->load(
          std::memory_order_relaxed);
      do {
        next_[slot_] = old;
      } while (!free_head_->compare_exchange_weak(
          old, slot_,
          std::memory_order_release,
          std::memory_order_relaxed));
    }
    data_ = o.data_;
    size_ = o.size_;
    capacity_ = o.capacity_;
    free_head_ = o.free_head_;
    next_ = o.next_;
    slot_ = o.slot_;
    o.slot_ = -1;
    o.data_ = nullptr;
  }
  return *this;
}

// -- FramePool ---------------------------------------------------------------

FramePool::FramePool(int count, int buf_size)
    : count_(count), buf_size_(buf_size) {
  storage_ = new uint8_t[
      static_cast<size_t>(count) * buf_size];
  next_ = new int[count];
  // Build freelist: 0 → 1 → 2 → ... → -1.
  for (int i = 0; i < count - 1; i++) {
    next_[i] = i + 1;
  }
  next_[count - 1] = -1;
  free_head_.store(0, std::memory_order_relaxed);
}

FramePool::~FramePool() {
  delete[] storage_;
  delete[] next_;
}

std::unique_ptr<FrameBuffer> FramePool::Alloc() {
  int slot = free_head_.load(std::memory_order_acquire);
  while (slot >= 0) {
    int next_slot = next_[slot];
    if (free_head_.compare_exchange_weak(
            slot, next_slot,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      uint8_t* data = storage_ +
          static_cast<size_t>(slot) * buf_size_;
      return std::unique_ptr<FrameBuffer>(
          new FrameBuffer(data, buf_size_,
                          &free_head_, next_, slot));
    }
  }
  return nullptr;  // Pool exhausted.
}

int FramePool::FreeCount() const {
  int count = 0;
  int slot = free_head_.load(std::memory_order_acquire);
  while (slot >= 0 && count < count_) {
    slot = next_[slot];
    count++;
  }
  return count;
}

}  // namespace hd::sdk
