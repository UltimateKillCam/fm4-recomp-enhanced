# In-order queue thread for non-presenting D3D12 submissions. Modified 2026-10-19.
# GPL-3.0-only. Applied as a build-local edit of the generated Gpu-Completion overlay.
# A submission that reaches an idle GPU can block inside ExecuteCommandLists for
# milliseconds. The command lists, their order, the queue and the fence values are
# unchanged; only the CPU thread that calls ExecuteCommandLists/Signal differs.
# Presenting submissions drain the thread and stay inline. Cvar: d3d12_async_submit.
function(fm4_async_submit)
  if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
    return()
  endif()
  set(fm4_file "${CMAKE_BINARY_DIR}/fm4-shader-completion/d3d_command_processor.cpp")
  if(NOT EXISTS "${fm4_file}")
    message(FATAL_ERROR "Async submit: generated D3D12 overlay is missing")
  endif()
  file(READ "${fm4_file}" fm4_text)
  string(FIND "${fm4_text}" "Fm4SubmitThread" fm4_done)
  if(fm4_done GREATER_EQUAL 0)
    return()
  endif()

  # Every anchor must occur exactly once in the generated source.
  macro(fm4_swap old new what)
    string(FIND "${fm4_text}" "${old}" fm4_at)
    string(FIND "${fm4_text}" "${old}" fm4_last REVERSE)
    if(fm4_at LESS 0)
      message(FATAL_ERROR "Async submit: ${what} anchor not found")
    endif()
    if(NOT fm4_at EQUAL fm4_last)
      message(FATAL_ERROR "Async submit: ${what} anchor is ambiguous")
    endif()
    string(REPLACE "${old}" "${new}" fm4_text "${fm4_text}")
  endmacro()

  # Enhanced builds also queue temporal/neural presentation work; those stay inline.
  set(fm4_temporal "")
  string(FIND "${fm4_text}" "Fm4TemporalHasQueuedFrame()" fm4_has_temporal)
  if(fm4_has_temporal GREATER_EQUAL 0)
    set(fm4_temporal " && !Fm4TemporalHasQueuedFrame()")
  endif()

  fm4_swap([=[#include <rex/ui/d3d12/d3d12_util.h>
]=] [=[#include <rex/ui/d3d12/d3d12_util.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

REXCVAR_DEFINE_BOOL(d3d12_async_submit, true, "GPU/D3D12",
                    "Call ExecuteCommandLists for non-presenting submissions on an in-order "
                    "queue thread so the command processor does not block inside the driver");

namespace {
// FM4 async submit. A submission that reaches an idle GPU can block inside
// ExecuteCommandLists for milliseconds. The command lists, their order, the
// queue and the fence values are unchanged; only the calling thread differs.
// Presenting submissions drain this thread and run inline, because the
// presenter signals its own fence on the same queue right after them.
class Fm4SubmitThread {
 public:
  static constexpr size_t kSlots = 8;
  struct Slot {
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList1* list_1 = nullptr;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> completion;
    uint64_t ticket = 0;
  };

  Slot* Acquire(ID3D12Device* device, ID3D12CommandAllocator* allocator) {
    Slot& slot = slots_[next_slot_++ % kSlots];
    if (slot.ticket) WaitFor(slot.ticket);
    if (!slot.list) {
      if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                           IID_PPV_ARGS(&slot.list)))) {
        return nullptr;
      }
      slot.list->SetName(L"FM4 guest rendering (queued)");
      slot.list->Close();
      slot.list->QueryInterface(IID_PPV_ARGS(&slot.list_1));
    }
    return &slot;
  }

  void Push(Slot& slot, ID3D12CommandQueue* queue, bool with_completion, ID3D12Fence* fence,
            uint64_t value) {
    std::lock_guard lock(mutex_);
    if (!thread_.joinable()) {
      stop_ = false;
      queue_ = queue;
      thread_ = std::thread([this] { Run(); });
    }
    slot.ticket = ++pushed_;
    items_.push_back({slot.list, with_completion ? slot.completion.Get() : nullptr, fence, value,
                      slot.ticket});
    work_cv_.notify_one();
  }

  void Drain() {
    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this] { return done_ >= pushed_; });
  }

  void Stop() {
    Drain();
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    work_cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    for (Slot& slot : slots_) {
      if (slot.list_1) slot.list_1->Release();
      if (slot.list) slot.list->Release();
      slot = {};
    }
  }

 private:
  struct Item {
    ID3D12CommandList* list;
    ID3D12CommandList* completion;
    ID3D12Fence* fence;
    uint64_t value;
    uint64_t ticket;
  };

  void WaitFor(uint64_t ticket) {
    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this, ticket] { return done_ >= ticket; });
  }

  void Run() {
    for (;;) {
      Item item;
      {
        std::unique_lock lock(mutex_);
        work_cv_.wait(lock, [this] { return stop_ || !items_.empty(); });
        if (items_.empty()) return;
        item = items_.front();
        items_.pop_front();
      }
      queue_->ExecuteCommandLists(1, &item.list);
      // Separate call: the completion marker list must observe all payload work.
      if (item.completion) queue_->ExecuteCommandLists(1, &item.completion);
      queue_->Signal(item.fence, item.value);
      {
        std::lock_guard lock(mutex_);
        done_ = item.ticket;
      }
      done_cv_.notify_all();
    }
  }

  Slot slots_[kSlots];
  size_t next_slot_ = 0;
  std::mutex mutex_;
  std::condition_variable work_cv_, done_cv_;
  std::deque<Item> items_;
  uint64_t pushed_ = 0, done_ = 0;
  bool stop_ = false;
  ID3D12CommandQueue* queue_ = nullptr;
  std::thread thread_;
};
Fm4SubmitThread fm4_submit_thread;
}  // namespace
]=] "include")

  fm4_swap([=[void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();]=] [=[void D3D12CommandProcessor::ShutdownContext() {
  fm4_submit_thread.Stop();
  AwaitAllQueueOperationsCompletion();]=] "shutdown")

  set(fm4_record [=[    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    // Queued submissions record into a ring slot's lists (swapped in here and
    // swapped back before queueing); inline ones first drain earlier queued work.
    Fm4SubmitThread::Slot* fm4_slot = nullptr;
    bool fm4_slot_completion = false;
    if (REXCVAR_GET(d3d12_async_submit) && !is_swap@FM4_TEMPORAL@)
      fm4_slot = fm4_submit_thread.Acquire(provider.GetDevice(), command_allocator);
    if (fm4_slot) {
      std::swap(command_list_, fm4_slot->list);
      std::swap(command_list_1_, fm4_slot->list_1);
      gpu_shader_completion_list_.Swap(fm4_slot->completion);
    } else {
      fm4_submit_thread.Drain();
    }
]=])
  string(REPLACE "@FM4_TEMPORAL@" "${fm4_temporal}" fm4_record "${fm4_record}")
  fm4_swap([=[    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
]=] "${fm4_record}" "record")

  # Queue block: Enhanced wraps it in a frame-metrics scope, Wheel does not.
  string(FIND "${fm4_text}" "fm4::frame_metrics::Scope stage(fm4::frame_metrics::Stage::Queue);" fm4_has_metrics)
  if(fm4_has_metrics GREATER_EQUAL 0)
    fm4_swap([=[    ID3D12CommandList* execute_command_lists[] = {command_list_};
    {
      fm4::frame_metrics::Scope stage(fm4::frame_metrics::Stage::Queue);
      direct_queue->ExecuteCommandLists(1, execute_command_lists);]=] [=[    ID3D12CommandList* execute_command_lists[] = {command_list_};
    if (fm4_slot) {
      fm4_slot_completion = !gpu_shader_completions_.empty();
      gpu_shader_completions_.clear();
    } else {
      fm4::frame_metrics::Scope stage(fm4::frame_metrics::Stage::Queue);
      direct_queue->ExecuteCommandLists(1, execute_command_lists);]=] "queue")
  else()
    fm4_swap([=[    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
      if (!gpu_shader_completions_.empty()) {
        ID3D12CommandList* completion_lists[] = {gpu_shader_completion_list_.Get()};
        direct_queue->ExecuteCommandLists(1, completion_lists);
        gpu_shader_completions_.clear();
      }
]=] [=[    ID3D12CommandList* execute_command_lists[] = {command_list_};
    if (fm4_slot) {
      fm4_slot_completion = !gpu_shader_completions_.empty();
      gpu_shader_completions_.clear();
    } else {
      direct_queue->ExecuteCommandLists(1, execute_command_lists);
      if (!gpu_shader_completions_.empty()) {
        ID3D12CommandList* completion_lists[] = {gpu_shader_completion_list_.Get()};
        direct_queue->ExecuteCommandLists(1, completion_lists);
        gpu_shader_completions_.clear();
      }
    }
]=] "queue")
  endif()

  fm4_swap([=[    direct_queue->Signal(submission_fence_, submission_current_++);

    submission_open_ = false;]=] [=[    if (fm4_slot) {
      std::swap(command_list_, fm4_slot->list);
      std::swap(command_list_1_, fm4_slot->list_1);
      gpu_shader_completion_list_.Swap(fm4_slot->completion);
      fm4_submit_thread.Push(*fm4_slot, direct_queue, fm4_slot_completion, submission_fence_,
                             submission_current_++);
    } else {
      direct_queue->Signal(submission_fence_, submission_current_++);
    }

    submission_open_ = false;]=] "signal")

  fm4_swap([=[void D3D12CommandProcessor::PrepareForDirectQueueOperation() {}]=]
    [=[void D3D12CommandProcessor::PrepareForDirectQueueOperation() {
  fm4_submit_thread.Drain();
}]=] "direct queue ordering")
  fm4_swap([=[      if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value)) &&]=]
    [=[      fm4_submit_thread.Drain();
      if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value)) &&]=]
    "out-of-submission signal ordering")

  file(WRITE "${fm4_file}" "${fm4_text}")
  message(STATUS "FM4 async submit applied")
endfunction()
fm4_async_submit()
