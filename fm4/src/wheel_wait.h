// Wheel polling timer. Modified 2026-10-18. GPL-3.0-only.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <chrono>
#include <thread>

namespace fm4::wheel {
// A timer owned by the polling thread avoids rounding a requested 4 ms poll
// interval up to the system's ~15.6 ms tick. No global timer setting is changed.
class PollTimer {
 public:
  PollTimer() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002,
                                  TIMER_MODIFY_STATE | SYNCHRONIZE);
    high_resolution_ = timer_ != nullptr;
    if (!timer_)
      timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0,
                                    TIMER_MODIFY_STATE | SYNCHRONIZE);
  }
  ~PollTimer() { if (timer_) CloseHandle(timer_); }
  PollTimer(const PollTimer&) = delete;
  PollTimer& operator=(const PollTimer&) = delete;
  bool high_resolution() const { return high_resolution_; }
  void Wait(std::chrono::milliseconds duration) const {
    if (duration.count() <= 0) return;
    LARGE_INTEGER due;
    due.QuadPart = -duration.count() * 10000;
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0) return;
    std::this_thread::sleep_for(duration);
  }
 private:
  HANDLE timer_ = nullptr;
  bool high_resolution_ = false;
};
}  // namespace fm4::wheel
