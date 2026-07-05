// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "pcm_ring_buffer.h"

#include <cassert>
#include <algorithm>

namespace voicestick {

PcmRingBuffer::PcmRingBuffer(std::size_t capacity_samples) {
  // 容量必须是 2 的幂，保证按位与可以替代取模。
  assert(capacity_samples > 0);
  assert((capacity_samples & (capacity_samples - 1)) == 0);
  buffer_.resize(capacity_samples);
  mask_ = capacity_samples - 1;
}

std::size_t PcmRingBuffer::Write(const int16_t* samples, std::size_t count) {
  if (samples == nullptr || count == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t capacity = buffer_.size();

  // 覆盖最旧数据：如果写入量超过容量，只保留最新的 capacity 个样本。
  const std::size_t effective_count = std::min(count, capacity);
  const int16_t* src = samples + (count - effective_count);

  for (std::size_t i = 0; i < effective_count; ++i) {
    buffer_[(write_pos_ + i) & mask_] = src[i];
  }

  write_pos_ = (write_pos_ + effective_count) & mask_;
  size_ = std::min(size_ + effective_count, capacity);

  // 如果发生覆盖，需要调整 read_pos_ 指向最旧的有效样本。
  if (effective_count == capacity) {
    read_pos_ = write_pos_;
    size_ = capacity;
  } else if (size_ == capacity) {
    read_pos_ = (write_pos_ - size_ + capacity) & mask_;
  }

  return effective_count;
}

std::size_t PcmRingBuffer::Read(int16_t* out, std::size_t count,
                                int16_t silence_value) {
  if (out == nullptr || count == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t capacity = buffer_.size();
  const std::size_t readable = std::min(size_, count);

  for (std::size_t i = 0; i < readable; ++i) {
    out[i] = buffer_[(read_pos_ + i) & mask_];
  }

  // 不足部分填充静音。
  if (readable < count) {
    std::fill(out + readable, out + count, silence_value);
  }

  read_pos_ = (read_pos_ + readable) & mask_;
  size_ -= readable;
  return readable;
}

void PcmRingBuffer::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  write_pos_ = 0;
  read_pos_ = 0;
  size_ = 0;
}

std::size_t PcmRingBuffer::Available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return size_;
}

std::size_t PcmRingBuffer::Capacity() const {
  return buffer_.size();
}

}  // namespace voicestick
