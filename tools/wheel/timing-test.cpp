// Guest-thread alertable timing/APC regressions. GPL-3.0-only, 2026-10-17.
#define NOMINMAX
#include <windows.h>
#include <rex/thread.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include "../../fm4/src/wheel_wait.h"

using Clock = std::chrono::steady_clock;
using Result = rex::thread::SleepResult;
static double Milliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
static void CALLBACK CountApc(ULONG_PTR address) {
  InterlockedIncrement(reinterpret_cast<volatile LONG*>(address));
}
struct WorkerState {
  HANDLE ready = nullptr;
  volatile LONG callbacks = 0;
  Result result = Result::kSuccess;
  double elapsed = 0, next_elapsed = 0;
};
static DWORD WINAPI Worker(void* argument) {
  auto& state = *static_cast<WorkerState*>(argument);
  SetEvent(state.ready);
  const auto start = Clock::now();
  state.result = rex::thread::AlertableSleep(std::chrono::seconds(3));
  state.elapsed = Milliseconds(start);
  const auto next = Clock::now();
  rex::thread::AlertableSleep(std::chrono::milliseconds(2));
  state.next_elapsed = Milliseconds(next);
  return 0;
}
int main() {
  try {
    fm4::wheel::PollTimer poll_timer;
    for (bool improved : {false, true}) {
      std::vector<double> samples;
      for (unsigned i = 0; i < 48; ++i) {
        const auto start = Clock::now();
        if (improved) poll_timer.Wait(std::chrono::milliseconds(4));
        else std::this_thread::sleep_for(std::chrono::milliseconds(4));
        const double elapsed = Milliseconds(start);
        Require(elapsed >= 3.5, "Wheel polling delay returned early");
        samples.push_back(elapsed);
      }
      std::sort(samples.begin(), samples.end());
      std::printf("%s wheel 4ms wait: p50=%.3fms p95=%.3fms max=%.3fms (high-resolution timer %s)\n",
          improved ? "Timer" : "Legacy", samples[24], samples[45], samples.back(),
          poll_timer.high_resolution() ? "available" : "unavailable");
    }
    for (bool improved : {false, true}) {
      std::vector<double> samples;
      for (unsigned i = 0; i < 48; ++i) {
        const auto start = Clock::now();
        if (improved) {
          Require(rex::thread::AlertableSleep(std::chrono::milliseconds(1)) == Result::kSuccess,
                  "Unrequested APC interrupted the duration check");
        } else {
          SleepEx(1, TRUE);
        }
        const double elapsed = Milliseconds(start);
        Require(elapsed >= .5, "A one-millisecond delay returned early");
        samples.push_back(elapsed);
      }
      std::sort(samples.begin(), samples.end());
      std::printf("%s 1ms sleep: p50=%.3fms p95=%.3fms max=%.3fms\n",
                  improved ? "SDK alertable" : "Legacy SleepEx", samples[24], samples[45], samples.back());
    }
    volatile LONG current_callbacks = 0;
    Require(QueueUserAPC(CountApc, GetCurrentThread(), reinterpret_cast<ULONG_PTR>(&current_callbacks)) != 0,
            "QueueUserAPC failed for zero-delay check");
    Require(rex::thread::AlertableSleep(std::chrono::microseconds(0)) == Result::kAlerted && current_callbacks == 1,
            "Zero delay lost the queued APC");
    Require(rex::thread::AlertableSleep(std::chrono::microseconds(-1)) == Result::kSuccess,
            "Nonpositive delay did not yield successfully");
    WorkerState state;
    state.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Require(state.ready != nullptr, "Could not create worker event");
    HANDLE worker = CreateThread(nullptr, 0, Worker, &state, 0, nullptr);
    Require(worker != nullptr, "Could not create APC worker");
    const DWORD ready = WaitForSingleObject(state.ready, 3000);
    const bool queued = ready == WAIT_OBJECT_0 && QueueUserAPC(
        CountApc, worker, reinterpret_cast<ULONG_PTR>(&state.callbacks)) != 0;
    const DWORD stopped = WaitForSingleObject(worker, 5000);
    CloseHandle(worker); CloseHandle(state.ready);
    Require(queued && stopped == WAIT_OBJECT_0, "APC worker did not finish");
    Require(state.result == Result::kAlerted && state.callbacks == 1,
            "Alertable wait did not deliver exactly one APC");
    Require(state.elapsed < 2500, "APC did not interrupt the three-second delay");
    Require(state.next_elapsed >= 1, "An interrupted timer leaked into the next delay");
    std::printf("PASS: duration, zero/nonpositive delay, APC interruption and next-wait isolation (APC %.3fms, next %.3fms).\n",
                state.elapsed, state.next_elapsed);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
  }
}
