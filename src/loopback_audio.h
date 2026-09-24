#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

class LoopbackAudioCapture {
public:
  using AudioCallback =
      std::function<void(int sample_rate, int channels, std::vector<std::int16_t>&&)>;

  ~LoopbackAudioCapture();

  void start(AudioCallback cb);
  void stop();

  int sample_rate() const noexcept { return sample_rate_.load(); }
  std::uint64_t blocks() const noexcept { return blocks_.load(); }

private:
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<int> sample_rate_{48000};
  std::atomic<std::uint64_t> blocks_{0};
};
