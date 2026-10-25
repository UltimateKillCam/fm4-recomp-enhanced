// Guest fence ordering across CPU-visible GPU writes and submission boundaries.
#pragma once
#include <cstdint>

namespace fm4::fences {
struct CompletionState {
  uint64_t cpu_write_submission = 0;
  uint64_t ordered_fence_submission = 0;

  void CpuWrite(uint64_t submission) {
    if (submission > cpu_write_submission) cpu_write_submission = submission;
  }
  void OrderedFence(uint64_t submission) {
    if (submission > ordered_fence_submission) ordered_fence_submission = submission;
  }
  uint64_t PendingSubmission() const {
    return cpu_write_submission > ordered_fence_submission
               ? cpu_write_submission : ordered_fence_submission;
  }
  bool CanCompleteEarly(uint64_t completed_submission) const {
    return PendingSubmission() <= completed_submission;
  }
};
}  // namespace fm4::fences
