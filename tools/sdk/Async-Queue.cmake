# Ordered queue operations for the optional CPU fast-path build.
# Runtime default remains off; the existing submitters are preserved verbatim.
if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
  return()
endif()
set(n3_path "${CMAKE_BINARY_DIR}/fm4-shader-completion/d3d_command_processor.cpp")
file(READ "${n3_path}" n3_text)
function(n3_replace variable old new label)
  string(FIND "${${variable}}" "${old}" first)
  string(FIND "${${variable}}" "${old}" last REVERSE)
  if(first LESS 0 OR NOT first EQUAL last)
    message(FATAL_ERROR "Async queue: ${label} anchor missing or ambiguous")
  endif()
  string(REPLACE "${old}" "${new}" result "${${variable}}")
  set(${variable} "${result}" PARENT_SCOPE)
endfunction()
n3_replace(n3_text "REXCVAR_DEFINE_BOOL(d3d12_recorder_thread, false,"
 [=[#include <functional>
REXCVAR_DEFINE_BOOL(n3_async_queue, false, "GPU/D3D12",
                    "Order sparse mappings, fence signals and swap submissions on the submit worker")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(d3d12_recorder_thread, false,]=] "switch")

# Give the experiment its own queue layout. Disabled builds take the unmodified
# legacy/recorder paths, including their original item size and allocation cost.
string(FIND "${n3_text}" "class Fm4RecorderSubmitThread {" n3_begin)
string(FIND "${n3_text}" "class Fm4SubmitThread {" n3_end)
if(n3_begin LESS 0 OR n3_end LESS n3_begin)
  message(FATAL_ERROR "Async queue: recorder class missing")
endif()
math(EXPR n3_length "${n3_end} - ${n3_begin}")
string(SUBSTRING "${n3_text}" ${n3_begin} ${n3_length} n3_class)
string(REPLACE "Fm4RecorderSubmitThread" "Fm4OrderedQueueThread" n3_class "${n3_class}")
n3_replace(n3_class [=[    uint64_t ticket;
  };]=] [=[    uint64_t ticket;
    std::function<void(ID3D12CommandQueue*)> operation = {};
  };]=] "operation ownership")

# Startup and publication are shared by tapes and queue-only operations. Both
# cross the recorder FIFO so a mapping cannot overtake an unrecorded tape.
n3_replace(n3_class [=[    if (!thread_.joinable()) {
      stop_ = false;
      queue_ = queue;
      thread_ = std::thread([this] { Run(); });
      if (REXCVAR_GET(d3d12_recorder_thread))
        recorder_ = std::thread([this] { Record(); });
    }
    slot.ticket = ++pushed_;]=] [=[    Start(queue);
    slot.ticket = ++pushed_;]=] "shared startup")
n3_replace(n3_class [=[  uint64_t PublishedTicket() {]=] [=[  uint64_t PushOperation(ID3D12CommandQueue* queue,
                         std::function<void(ID3D12CommandQueue*)> operation) {
    std::lock_guard lock(mutex_);
    Start(queue);
    Item item{nullptr, false, nullptr, 0, ++pushed_, std::move(operation)};
    const uint64_t ticket = item.ticket;
    if (recorder_.joinable()) {
      record_items_.push_back(std::move(item));
      record_cv_.notify_one();
    } else {
      items_.push_back(std::move(item));
      work_cv_.notify_one();
    }
    return ticket;
  }
  uint64_t PublishedTicket() {]=] "queue-only publication")
n3_replace(n3_class [=[  void WaitFor(uint64_t ticket) {]=] [=[  // Called with mutex_ held. Queue selection is fixed for this lifetime.
  void Start(ID3D12CommandQueue* queue) {
    if (thread_.joinable()) {
      assert_true(queue_ == queue);
      return;
    }
    stop_ = false;
    queue_ = queue;
    REXGPU_INFO("D3D12 ordered queue active: submission, sparse mapping and signal tickets");
    thread_ = std::thread([this] { Run(); });
    if (REXCVAR_GET(d3d12_recorder_thread))
      recorder_ = std::thread([this] { Record(); });
  }
  void WaitFor(uint64_t ticket) {]=] "worker startup")
# Queue-only payloads have no slot and need no recording.
n3_replace(n3_class [=[      Slot& slot = *item.slot;
      if (slot.record) {]=] [=[      if (item.slot && item.slot->record) {
        Slot& slot = *item.slot;]=] "record-only payload")
n3_replace(n3_class [=[      Slot& slot = *item.slot;
      Fm4RecorderTiming timing(Fm4RecorderTiming::WorkerExecute);]=] [=[      Fm4RecorderTiming timing(Fm4RecorderTiming::WorkerExecute);
      if (item.operation) {
        item.operation(queue_);
      } else {
      Slot& slot = *item.slot;]=] "operation dispatch")
n3_replace(n3_class [=[      queue_->Signal(item.fence, item.value);
      {]=] [=[      if (FAILED(queue_->Signal(item.fence, item.value))) {
        REXGPU_ERROR("Failed to signal an ordered submission fence");
        std::abort();
      }
      }
      // A CPU ticket retires only after the queue call has returned. GPU fence
      // retirement still owns allocators, descriptors and readback resources.
      {]=] "operation completion")
# Move operations through both FIFOs, retaining COM ownership without copies.
string(REPLACE "item = items_.front();" "item = std::move(items_.front());" n3_class "${n3_class}")
string(REPLACE "item = record_items_.front();" "item = std::move(record_items_.front());" n3_class "${n3_class}")
string(REPLACE "items_.push_back(item);" "items_.push_back(std::move(item));" n3_class "${n3_class}")
string(REPLACE "record_items_.push_back(item);" "record_items_.push_back(std::move(item));" n3_class "${n3_class}")

# The facade selects the experimental queue only for the new restart-only cvar.
n3_replace(n3_text "class Fm4SubmitThread {" "${n3_class}class Fm4SubmitThread {" "isolated queue")
n3_replace(n3_text [=[    if (REXCVAR_GET(d3d12_recorder_thread)) return recorder_.Acquire(device, allocator);]=]
 [=[    if (REXCVAR_GET(n3_async_queue)) return ordered_.Acquire(device, allocator);
    if (REXCVAR_GET(d3d12_recorder_thread)) return recorder_.Acquire(device, allocator);]=] "acquire routing")
n3_replace(n3_text [=[    if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.Push(slot, queue, completion, fence, value);]=]
 [=[    if (REXCVAR_GET(n3_async_queue)) ordered_.Push(slot, queue, completion, fence, value);
    else if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.Push(slot, queue, completion, fence, value);]=] "submission routing")
n3_replace(n3_text [=[    return REXCVAR_GET(d3d12_recorder_thread) ? recorder_.PublishedTicket()]=]
 [=[    if (REXCVAR_GET(n3_async_queue)) return ordered_.PublishedTicket();
    return REXCVAR_GET(d3d12_recorder_thread) ? recorder_.PublishedTicket()]=] "ticket routing")
n3_replace(n3_text [=[    if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.WaitThrough(ticket);]=]
 [=[    if (REXCVAR_GET(n3_async_queue)) ordered_.WaitThrough(ticket);
    else if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.WaitThrough(ticket);]=] "wait routing")
n3_replace(n3_text [=[    if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.Drain();]=]
 [=[    if (REXCVAR_GET(n3_async_queue)) ordered_.Drain();
    else if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.Drain();]=] "drain routing")
n3_replace(n3_text [=[  void Stop() { recorder_.Stop(); legacy_.Stop(); }]=]
 [=[  uint64_t PushOperation(ID3D12CommandQueue* queue,
                         std::function<void(ID3D12CommandQueue*)> operation) {
    return ordered_.PushOperation(queue, std::move(operation));
  }
  void Stop() { ordered_.Stop(); recorder_.Stop(); legacy_.Stop(); }]=] "operation routing")
n3_replace(n3_text [=[  Fm4LegacySubmitThread legacy_;]=]
 [=[  Fm4OrderedQueueThread ordered_;
  Fm4LegacySubmitThread legacy_;]=] "queue lifetime")

# Shutdown may end one last open submission or signal trailing mappings. Keep
# the workers alive until those operations and their GPU fences have retired.
n3_replace(n3_text [=[  fm4_submit_thread.Stop();
  AwaitAllQueueOperationsCompletion();]=]
 [=[  if (!REXCVAR_GET(n3_async_queue)) fm4_submit_thread.Stop();
  AwaitAllQueueOperationsCompletion();
  if (REXCVAR_GET(n3_async_queue)) fm4_submit_thread.Stop();]=] "shutdown retirement")

n3_replace(n3_text [=[    if (REXCVAR_GET(d3d12_async_submit) && !is_swap && !Fm4TemporalHasQueuedFrame())]=]
 [=[    if (REXCVAR_GET(d3d12_async_submit) &&
        (!is_swap || REXCVAR_GET(n3_async_queue)) && !Fm4TemporalHasQueuedFrame())]=] "swap queueing")
# EndSubmission may close capture and reclaim frame state. Retire the fixed CPU
# ticket before that and before the refresh callback returns to the presenter,
# whose own queue signal and mailbox publication must follow our Execute/Signal.
n3_replace(n3_text [=[  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state)]=]
 [=[  if (is_closing_frame && REXCVAR_GET(n3_async_queue)) {
    fm4_submit_thread.WaitThrough(fm4_submit_thread.PublishedTicket());
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state)]=] "present boundary")

n3_replace(n3_text [=[  PrepareForDirectQueueOperation();
  GetD3D12Provider().GetDirectQueue()->UpdateTileMappings(
      resource, 1, &coordinate, &region, heap, 1, &flags, &heap_offset, &tile_count,
      D3D12_TILE_MAPPING_FLAG_NONE);
  NotifyQueueOperationsDoneDirectly();]=]
 [=[  if (REXCVAR_GET(n3_async_queue)) {
    // Copy stack descriptors and retain the objects until the driver consumes
    // the mapping. Subsequent GPU work is protected by the existing fences.
    Microsoft::WRL::ComPtr<ID3D12Resource> owned_resource = resource;
    Microsoft::WRL::ComPtr<ID3D12Heap> owned_heap = heap;
    fm4_submit_thread.PushOperation(GetD3D12Provider().GetDirectQueue(),
        [owned_resource, owned_heap, coordinate, region, flags, heap_offset, tile_count]
        (ID3D12CommandQueue* queue) {
          queue->UpdateTileMappings(owned_resource.Get(), 1, &coordinate, &region,
              owned_heap.Get(), 1, &flags, &heap_offset, &tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
        });
  } else {
    PrepareForDirectQueueOperation();
    GetD3D12Provider().GetDirectQueue()->UpdateTileMappings(
        resource, 1, &coordinate, &region, heap, 1, &flags, &heap_offset, &tile_count,
        D3D12_TILE_MAPPING_FLAG_NONE);
  }
  NotifyQueueOperationsDoneDirectly();]=] "sparse mapping ownership")

n3_replace(n3_text [=[      { Fm4RecorderTiming timing(Fm4RecorderTiming::DrainFence);
        if (REXCVAR_GET(d3d12_ticketed_submit_ordering))
          fm4_submit_thread.WaitThrough(fm4_submit_thread.PublishedTicket());
        else fm4_submit_thread.Drain(); }
      if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value)) &&]=]
 [=[      HRESULT fm4_signal_result = S_OK;
      if (REXCVAR_GET(n3_async_queue)) {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence = queue_operations_since_submission_fence_;
        fm4_submit_thread.PushOperation(direct_queue, [fence, fence_value](ID3D12CommandQueue* queue) {
          if (FAILED(queue->Signal(fence.Get(), fence_value))) {
            REXGPU_ERROR("Failed to signal an ordered sparse mapping completion fence");
            std::abort();
          }
        });
      } else {
        { Fm4RecorderTiming timing(Fm4RecorderTiming::DrainFence);
          if (REXCVAR_GET(d3d12_ticketed_submit_ordering))
            fm4_submit_thread.WaitThrough(fm4_submit_thread.PublishedTicket());
          else fm4_submit_thread.Drain(); }
        fm4_signal_result = direct_queue->Signal(queue_operations_since_submission_fence_, fence_value);
      }
      if (SUCCEEDED(fm4_signal_result) &&]=] "standalone fence ownership")
# The presenter's own resource fence stays inline before mailbox publication.
# Deferring it past that boundary lets UI painting overtake the signal and can
# couple later guest dispatch to host presentation latency.
file(WRITE "${n3_path}" "${n3_text}")
message(STATUS "FM4 ordered async queue applied (runtime default off)")
