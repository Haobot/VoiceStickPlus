// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 线程安全的 16-bit PCM 环形缓冲区。
// 用于 wechat_input_method 模式下，解耦 BLE 音频到达线程与 WASAPI 渲染线程。

#ifndef VOICESTICK_PCM_RING_BUFFER_H_
#define VOICESTICK_PCM_RING_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace voicestick {

// 线程安全的单生产者单消费者环形缓冲区（基于 std::mutex，适合低频音频帧）。
// 所有样本按 int16_t 存储，容量在构造时固定。
class PcmRingBuffer {
 public:
  // capacity_samples 必须是 2 的幂，便于取模运算。
  explicit PcmRingBuffer(std::size_t capacity_samples);

  // 写入样本。当空间不足时覆盖最旧数据（drop-oldest）。
  // 返回实际写入的样本数。
  std::size_t Write(const int16_t* samples, std::size_t count);

  // 读取样本。当数据不足时用 silence_value 填充剩余位置。
  // 返回实际从缓冲中读出的样本数（不含填充的静音）。
  std::size_t Read(int16_t* out, std::size_t count, int16_t silence_value = 0);

  // 清空缓冲区。
  void Clear();

  // 当前可读样本数。
  std::size_t Available() const;

  // 缓冲区总容量（样本数）。
  std::size_t Capacity() const;

 private:
  mutable std::mutex mutex_;
  std::vector<int16_t> buffer_;
  std::size_t mask_ = 0;
  std::size_t write_pos_ = 0;
  std::size_t read_pos_ = 0;
  std::size_t size_ = 0;
};

}  // namespace voicestick

#endif  // VOICESTICK_PCM_RING_BUFFER_H_
