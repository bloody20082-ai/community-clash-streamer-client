#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

class LoopbackAudio {
public:
  using AudioCallback =
      std::function<void(std::vector<std::int16_t>&&, unsigned, unsigned)>;

  ~LoopbackAudio();
  void start(AudioCallback cb);
  void stop();

private:
  std::atomic<bool> running_{false};
  std::thread thread_;
};
