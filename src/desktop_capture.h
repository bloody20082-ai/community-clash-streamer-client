#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

struct DesktopOutput {
  unsigned adapter_index = 0;
  unsigned output_index = 0;
  std::wstring name;
  unsigned width = 0;
  unsigned height = 0;
  long left = 0;
  long top = 0;
  long right = 0;
  long bottom = 0;
};

class DesktopCapture {
public:
  using FrameCallback =
      std::function<void(unsigned, unsigned, std::vector<std::uint8_t>&&, std::int64_t)>;

  static std::vector<DesktopOutput> enumerate();

  ~DesktopCapture();
  void start(const DesktopOutput& output, FrameCallback cb);
  void start_window(HWND hwnd, FrameCallback cb);
  void stop();

  double fps() const noexcept { return fps_.load(); }
  std::uint64_t frames() const noexcept { return frames_.load(); }

private:
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<double> fps_{0.0};
  std::atomic<std::uint64_t> frames_{0};
};
