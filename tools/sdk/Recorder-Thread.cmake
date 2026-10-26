# Optional recording worker for the existing immutable deferred command stream.
# Applied after submission and pass-timing overlays.
if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
  return()
endif()
set(fm4_recorder_path "${CMAKE_BINARY_DIR}/fm4-shader-completion/d3d_command_processor.cpp")
file(READ "${fm4_recorder_path}" fm4_recorder_text)
macro(fm4_recorder_replace old new label)
  string(FIND "${fm4_recorder_text}" "${old}" fm4_recorder_first)
  string(FIND "${fm4_recorder_text}" "${old}" fm4_recorder_last REVERSE)
  if(fm4_recorder_first LESS 0 OR NOT fm4_recorder_first EQUAL fm4_recorder_last)
    message(FATAL_ERROR "Async recording: ${label} anchor missing or ambiguous")
  endif()
  string(REPLACE "${old}" "${new}" fm4_recorder_text "${fm4_recorder_text}")
endmacro()
fm4_recorder_replace([=[namespace {
// FM4 async submit.]=] [=[REXCVAR_DEFINE_BOOL(d3d12_recorder_thread, false, "GPU/D3D12",
                    "Record non-presenting deferred submissions on a separate worker; "
                    "requires d3d12_async_submit")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
// FM4 async submit.]=] "switch")
fm4_recorder_replace([=[    uint64_t ticket = 0;
  };]=] [=[    uint64_t ticket = 0;
    bool record = false;
    ID3D12CommandAllocator* allocator = nullptr;
    std::unique_ptr<rex::graphics::d3d12::DeferredCommandList> prefix, commands, suffix, markers;
  };]=] "slot ownership")
fm4_recorder_replace([=[    if (slot.ticket) WaitFor(slot.ticket);
]=] [=[    if (slot.ticket) WaitFor(slot.ticket);
    slot.record = false;
]=] "slot reuse")
fm4_recorder_replace([=[    items_.push_back({slot.list, with_completion ? slot.completion.Get() : nullptr, fence, value,
                      slot.ticket});]=] [=[    items_.push_back({&slot, with_completion, fence, value, slot.ticket});]=] "publication")
fm4_recorder_replace([=[    ID3D12CommandList* list;
    ID3D12CommandList* completion;
    ID3D12Fence* fence;]=] [=[    Slot* slot;
    bool with_completion;
    ID3D12Fence* fence;]=] "queue item")
fm4_recorder_replace([=[      queue_->ExecuteCommandLists(1, &item.list);
      // Separate call: the completion marker list must observe all payload work.
      if (item.completion) queue_->ExecuteCommandLists(1, &item.completion);]=] [=[      Slot& slot = *item.slot;
      if (slot.record) {
        if (!recorded_once_) {
          REXGPU_INFO("D3D12 recorder thread active: immutable submission replay");
          recorded_once_ = true;
        }
        // The decoder has detached this stream and allocator. Their lifetime
        // remains tied to this submission's GPU fence, not the CPU ticket.
        if (FAILED(slot.allocator->Reset()) ||
            FAILED(slot.list->Reset(slot.allocator, nullptr))) {
          REXGPU_ERROR("Deferred recording reset failed");
          std::abort();
        }
        slot.prefix->Execute(slot.list, slot.list_1);
        slot.commands->Execute(slot.list, slot.list_1);
        slot.suffix->Execute(slot.list, slot.list_1);
        if (FAILED(slot.list->Close())) {
          REXGPU_ERROR("Deferred recording close failed");
          std::abort();
        }
        if (item.with_completion) {
          HRESULT result;
          if (slot.completion) {
            result = slot.completion->Reset(slot.allocator, nullptr);
          } else {
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            result = slot.list->GetDevice(IID_PPV_ARGS(&device));
            if (SUCCEEDED(result))
              result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                  slot.allocator, nullptr, IID_PPV_ARGS(&slot.completion));
          }
          if (FAILED(result)) {
            REXGPU_ERROR("Deferred marker recording reset failed");
            std::abort();
          }
          slot.markers->Execute(slot.completion.Get(), nullptr);
          if (FAILED(slot.completion->Close())) {
            REXGPU_ERROR("Deferred marker recording close failed");
            std::abort();
          }
        }
      }
      ID3D12CommandList* payload = slot.list;
      queue_->ExecuteCommandLists(1, &payload);
      // Separate call: the completion marker list must observe all payload work.
      if (item.with_completion) {
        ID3D12CommandList* marker = slot.completion.Get();
        queue_->ExecuteCommandLists(1, &marker);
      }]=] "worker recording")
fm4_recorder_replace([=[    if (fm4_slot) {
      std::swap(command_list_, fm4_slot->list);
      std::swap(command_list_1_, fm4_slot->list_1);
      gpu_shader_completion_list_.Swap(fm4_slot->completion);
    } else {
      fm4_submit_thread.Drain();
    }
    {
      fm4::frame_metrics::Scope stage(fm4::frame_metrics::Stage::Record);]=] [=[    const bool fm4_record_async = fm4_slot && REXCVAR_GET(d3d12_recorder_thread) &&
                                  !pix_capturing_;
    if (fm4_slot && !fm4_record_async) {
      std::swap(command_list_, fm4_slot->list);
      std::swap(command_list_1_, fm4_slot->list_1);
      gpu_shader_completion_list_.Swap(fm4_slot->completion);
    } else if (!fm4_slot) {
      fm4_submit_thread.Drain();
    }
    if (fm4_record_async) {
      fm4::frame_metrics::Scope stage(fm4::frame_metrics::Stage::Record);
      using Stream = DeferredCommandList;
      if (!fm4_slot->commands) {
        fm4_slot->prefix = std::make_unique<Stream>(*this, 0);
        fm4_slot->commands = std::make_unique<Stream>(*this, 0);
        fm4_slot->suffix = std::make_unique<Stream>(*this, 0);
        fm4_slot->markers = std::make_unique<Stream>(*this, 0);
      }
      fm4_slot->prefix->Reset();
      fm4_slot->suffix->Reset();
      fm4_slot->markers->Reset();
      // Timestamp bookkeeping stays entirely on the decoder. Only immutable
      // timestamp commands cross the queue, so its profiling rings remain local.
      const UINT timing = fm4::frame_metrics::Gpu().Begin(provider.GetDevice(), direct_queue,
          fm4_slot->prefix.get(), submission_completed_);
      fm4::frame_metrics::Passes().EndSubmission(fm4_slot->suffix.get(), submission_current_);
      fm4::frame_metrics::Gpu().End(fm4_slot->suffix.get(), timing, submission_current_);
      deferred_command_list_.ResolvePipelineStates();
      deferred_command_list_.SwapCommands(*fm4_slot->commands);
      for (const auto& write : gpu_shader_completions_)
        fm4_slot->markers->D3DCopyBufferRegion(shared_memory_->GetReadbackResolveHostBuffer(),
            write.address, write.source, write.offset, sizeof(uint32_t));
      fm4_slot->allocator = command_allocator;
      fm4_slot->record = true;
    } else {
    {
      fm4::frame_metrics::Scope stage(fm4::frame_metrics::Stage::Record);]=] "stream handoff")
fm4_recorder_replace([=[    ID3D12CommandList* execute_command_lists[] = {command_list_};
    if (fm4_slot) {]=] [=[    }
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    if (fm4_slot) {]=] "inline recording scope")
fm4_recorder_replace([=[    if (fm4_slot) {
      std::swap(command_list_, fm4_slot->list);
      std::swap(command_list_1_, fm4_slot->list_1);
      gpu_shader_completion_list_.Swap(fm4_slot->completion);
      fm4_submit_thread.Push]=] [=[    if (fm4_slot) {
      if (!fm4_record_async) {
        std::swap(command_list_, fm4_slot->list);
        std::swap(command_list_1_, fm4_slot->list_1);
        gpu_shader_completion_list_.Swap(fm4_slot->completion);
      }
      fm4_submit_thread.Push]=] "recording ownership")
fm4_recorder_replace([=[  Slot slots_[kSlots];]=] [=[  bool recorded_once_ = false;
  Slot slots_[kSlots];]=] "activation diagnostic")
fm4_recorder_replace([=[    if (thread_.joinable()) thread_.join();]=] [=[    if (thread_.joinable()) thread_.join();
    recorded_once_ = false;]=] "diagnostic reset")

# Preserve the original eight-slot submitter before applying recorder-only
# diagnostics and queue changes. Both paths share its unchanged slot layout.
string(FIND "${fm4_recorder_text}" "class Fm4SubmitThread {" fm4_legacy_begin)
string(FIND "${fm4_recorder_text}" "Fm4SubmitThread fm4_submit_thread;" fm4_legacy_end)
math(EXPR fm4_legacy_length "${fm4_legacy_end} - ${fm4_legacy_begin}")
if(fm4_legacy_begin LESS 0 OR fm4_legacy_length LESS 1)
  message(FATAL_ERROR "Recorder legacy submitter anchors missing")
endif()
string(SUBSTRING "${fm4_recorder_text}" ${fm4_legacy_begin} ${fm4_legacy_length} fm4_legacy_class)
file(WRITE "${CMAKE_BINARY_DIR}/fm4-shader-completion/recorder-stage1-class.inc" "${fm4_legacy_class}")

# Per-thread counters avoid shared profiling locks. Each line covers one second.
fm4_recorder_replace([=[class Fm4SubmitThread {]=] [=[class Fm4RecorderTiming {
 public:
  enum Site { Acquire, Ring, PushLock, DrainInline, DrainDirect, DrainFence,
              Allocator, Resolve, Handoff, WorkerReset, WorkerRecord, WorkerExecute, Count };
  explicit Fm4RecorderTiming(Site site) : site_(site),
      enabled_(REXCVAR_GET(d3d12_recorder_thread) &&
          (!REXCVAR_GET(n1_hotpath_cache) || Requested())) {
    if (enabled_) QueryPerformanceCounter(&start_);
  }
  ~Fm4RecorderTiming() {
    if (!enabled_) return;
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    Counters* local = GetCounters();
    if (!local) return;
    Counters& counters = *local;
    counters.ticks[site_] += end.QuadPart - start_.QuadPart;
    ++counters.calls[site_];
    if (!counters.last) counters.last = end.QuadPart;
    if (end.QuadPart - counters.last < Frequency()) return;
    static const char* names[] = {"acquire", "ring_wait", "push_lock", "drain_inline",
        "drain_direct", "drain_fence", "allocator_create", "pso_resolve", "handoff",
        "worker_reset", "worker_record", "worker_execute"};
    for (size_t i = 0; i < Count; ++i) {
      if (counters.calls[i])
        REXGPU_INFO("FM4 recorder waits: site={} calls={} ms={:.3f} seconds={:.3f}",
            names[i], counters.calls[i], double(counters.ticks[i]) * 1000.0 / Frequency(),
            double(end.QuadPart - counters.last) / Frequency());
    }
    counters = {};
    counters.last = end.QuadPart;
  }
 private:
  static bool Requested() {
    static const bool requested = std::getenv("FM4_RECORDER_TIMING") != nullptr;
    return requested;
  }
  struct Counters { int64_t ticks[Count] = {}; uint64_t calls[Count] = {}; int64_t last = 0; };
  static Counters* GetCounters() {
    // Allocate diagnostics only when enabled. Do not change the module's static
    // TLS layout for the legacy path merely by compiling the recorder.
    struct Storage {
      DWORD index = FlsAlloc([](void* value) { delete static_cast<Counters*>(value); });
      ~Storage() { if (index != FLS_OUT_OF_INDEXES) FlsFree(index); }
    };
    static Storage storage;
    if (storage.index == FLS_OUT_OF_INDEXES) return nullptr;
    auto* value = static_cast<Counters*>(FlsGetValue(storage.index));
    if (!value) {
      value = new (std::nothrow) Counters;
      if (!value || !FlsSetValue(storage.index, value)) {
        delete value;
        return nullptr;
      }
    }
    return value;
  }
  static int64_t Frequency() {
    static const int64_t frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return frequency;
  }
  Site site_;
  bool enabled_;
  LARGE_INTEGER start_ = {};
};
class Fm4SubmitThread {]=] "wait diagnostics")
fm4_recorder_replace([=[  Slot* Acquire(ID3D12Device* device, ID3D12CommandAllocator* allocator) {
]=] [=[  Slot* Acquire(ID3D12Device* device, ID3D12CommandAllocator* allocator) {
    Fm4RecorderTiming timing(Fm4RecorderTiming::Acquire);
]=] "acquire timing")
fm4_recorder_replace([=[    std::lock_guard lock(mutex_);
    if (!thread_.joinable())]=] [=[    std::unique_lock lock(mutex_, std::defer_lock);
    { Fm4RecorderTiming timing(Fm4RecorderTiming::PushLock); lock.lock(); }
    if (!thread_.joinable())]=] "push mutex timing")
fm4_recorder_replace([=[    if (slot.ticket) WaitFor(slot.ticket);]=] [=[    if (slot.ticket) {
      Fm4RecorderTiming timing(Fm4RecorderTiming::Ring);
      WaitFor(slot.ticket);
    }]=] "ring timing")
fm4_recorder_replace([=[    } else if (!fm4_slot) {
      fm4_submit_thread.Drain();]=] [=[    } else if (!fm4_slot) {
      Fm4RecorderTiming timing(Fm4RecorderTiming::DrainInline);
      fm4_submit_thread.Drain();]=] "inline drain timing")
fm4_recorder_replace([=[void D3D12CommandProcessor::PrepareForDirectQueueOperation() {
  fm4_submit_thread.Drain();]=] [=[void D3D12CommandProcessor::PrepareForDirectQueueOperation() {
  Fm4RecorderTiming timing(Fm4RecorderTiming::DrainDirect);
  fm4_submit_thread.Drain();]=] "direct drain timing")
fm4_recorder_replace([=[      fm4_submit_thread.Drain();
      if (SUCCEEDED(direct_queue->Signal]=] [=[      { Fm4RecorderTiming timing(Fm4RecorderTiming::DrainFence); fm4_submit_thread.Drain(); }
      if (SUCCEEDED(direct_queue->Signal]=] "fence drain timing")
fm4_recorder_replace([=[  if (submission_open_ && !command_allocator_writable_first_) {
]=] [=[  if (submission_open_ && !command_allocator_writable_first_) {
    Fm4RecorderTiming timing(Fm4RecorderTiming::Allocator);
]=] "allocator timing")
fm4_recorder_replace([=[      deferred_command_list_.ResolvePipelineStates();]=] [=[      { Fm4RecorderTiming timing(Fm4RecorderTiming::Resolve);
        deferred_command_list_.ResolvePipelineStates(); }
      Fm4RecorderTiming timing_handoff(Fm4RecorderTiming::Handoff);]=] "resolve timing")
fm4_recorder_replace([=[        if (FAILED(slot.allocator->Reset()) ||]=] [=[        {
        Fm4RecorderTiming timing(Fm4RecorderTiming::WorkerReset);
        if (FAILED(slot.allocator->Reset()) ||]=] "worker reset timing")
fm4_recorder_replace([=[        slot.prefix->Execute(slot.list, slot.list_1);]=] [=[        }
        Fm4RecorderTiming timing(Fm4RecorderTiming::WorkerRecord);
        slot.prefix->Execute(slot.list, slot.list_1);]=] "worker record timing")
fm4_recorder_replace([=[      ID3D12CommandList* payload = slot.list;]=] [=[      Fm4RecorderTiming timing(Fm4RecorderTiming::WorkerExecute);
      ID3D12CommandList* payload = slot.list;]=] "worker execute timing")


# Replay and driver submission must overlap; a blocked ExecuteCommandLists must
# not prevent recording the next immutable tape.
fm4_recorder_replace([=[  static constexpr size_t kSlots = 8;]=] [=[  static constexpr size_t kSlots = 32;]=] "pipeline capacity")
fm4_recorder_replace([=[      thread_ = std::thread([this] { Run(); });
]=] [=[      thread_ = std::thread([this] { Run(); });
      if (REXCVAR_GET(d3d12_recorder_thread))
        recorder_ = std::thread([this] { Record(); });
]=] "recorder startup")
fm4_recorder_replace([=[    items_.push_back({&slot, with_completion, fence, value, slot.ticket});
    work_cv_.notify_one();]=] [=[    Item item{&slot, with_completion, fence, value, slot.ticket};
    if (recorder_.joinable()) {
      record_items_.push_back(item);
      record_cv_.notify_one();
    } else {
      items_.push_back(item);
      work_cv_.notify_one();
    }]=] "ordered recording publication")
fm4_recorder_replace([=[    work_cv_.notify_one();
    if (thread_.joinable()) thread_.join();]=] [=[    work_cv_.notify_one();
    record_cv_.notify_one();
    if (recorder_.joinable()) recorder_.join();
    if (thread_.joinable()) thread_.join();]=] "recorder shutdown")
# Extract the immutable replay block from the submission loop without changing it.
set(fm4_record_begin "      Slot& slot = *item.slot;\n      if (slot.record) {")
set(fm4_record_end "      Fm4RecorderTiming timing(Fm4RecorderTiming::WorkerExecute);")
string(FIND "${fm4_recorder_text}" "${fm4_record_begin}" fm4_record_start)
string(FIND "${fm4_recorder_text}" "${fm4_record_end}" fm4_record_finish)
if(fm4_record_start LESS 0 OR fm4_record_finish LESS fm4_record_start)
  message(FATAL_ERROR "Recorder: replay extraction anchors missing")
endif()
math(EXPR fm4_record_length "${fm4_record_finish} - ${fm4_record_start}")
string(SUBSTRING "${fm4_recorder_text}" ${fm4_record_start} ${fm4_record_length} fm4_replay)
fm4_recorder_replace("${fm4_replay}" "      Slot& slot = *item.slot;\n" "detach replay")
set(fm4_record_loop [=[  void Record() {
    for (;;) {
      Item item;
      {
        std::unique_lock lock(mutex_);
        record_cv_.wait(lock, [this] { return stop_ || !record_items_.empty(); });
        if (record_items_.empty()) return;
        item = record_items_.front();
        record_items_.pop_front();
      }
@REPLAY@
      // All items, including producer-recorded PIX lists, cross this FIFO.
      // Publish only after Close, before the submitter touches either list.
      {
        std::lock_guard lock(mutex_);
        items_.push_back(item);
      }
      work_cv_.notify_one();
    }
  }

  void Run() {]=])
string(REPLACE "@REPLAY@" "${fm4_replay}" fm4_record_loop "${fm4_record_loop}")
fm4_recorder_replace("  void Run() {" "${fm4_record_loop}" "recording loop")
fm4_recorder_replace([=[  std::condition_variable work_cv_, done_cv_;
  std::deque<Item> items_;]=] [=[  std::condition_variable work_cv_, record_cv_, done_cv_;
  std::deque<Item> items_, record_items_;]=] "recorder queue")
fm4_recorder_replace([=[  std::thread thread_;]=] [=[  std::thread thread_, recorder_;]=] "recorder lifetime")

fm4_recorder_replace([=[    Slot& slot = slots_[next_slot_++ % kSlots];]=] [=[    const size_t capacity = REXCVAR_GET(d3d12_recorder_thread) ? kSlots : 8;
    Slot& slot = slots_[next_slot_++ % capacity];]=] "legacy queue capacity")

# OFF calls the exact stage-one queue implementation. New slot capacity, thread
# startup, FIFO publication and timing code belong exclusively to the ON queue.
string(FIND "${fm4_recorder_text}" "class Fm4SubmitThread {" fm4_new_begin)
string(FIND "${fm4_recorder_text}" "Fm4SubmitThread fm4_submit_thread;" fm4_new_end)
math(EXPR fm4_new_length "${fm4_new_end} - ${fm4_new_begin}")
string(SUBSTRING "${fm4_recorder_text}" ${fm4_new_begin} ${fm4_new_length} fm4_new_class)
string(REPLACE "Fm4SubmitThread" "Fm4LegacySubmitThread" fm4_legacy_named "${fm4_legacy_class}")
string(REPLACE "Fm4SubmitThread" "Fm4RecorderSubmitThread" fm4_new_named "${fm4_new_class}")
string(FIND "${fm4_legacy_class}" "  struct Slot {" fm4_slot_begin)
string(SUBSTRING "${fm4_legacy_class}" ${fm4_slot_begin} -1 fm4_slot_tail)
string(FIND "${fm4_slot_tail}" "  };" fm4_slot_end)
math(EXPR fm4_slot_length "${fm4_slot_end} + 4")
string(SUBSTRING "${fm4_slot_tail}" 0 ${fm4_slot_length} fm4_slot_declaration)
string(REPLACE "${fm4_slot_declaration}" "  using Slot = Fm4LegacySubmitThread::Slot;"
       fm4_new_named "${fm4_new_named}")
set(fm4_queue_facade [=[class Fm4SubmitThread {
 public:
  using Slot = Fm4LegacySubmitThread::Slot;
  Slot* Acquire(ID3D12Device* device, ID3D12CommandAllocator* allocator) {
    if (REXCVAR_GET(d3d12_recorder_thread)) return recorder_.Acquire(device, allocator);
    return legacy_.Acquire(device, allocator);
  }
  void Push(Slot& slot, ID3D12CommandQueue* queue, bool completion, ID3D12Fence* fence,
            uint64_t value) {
    if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.Push(slot, queue, completion, fence, value);
    else legacy_.Push(slot, queue, completion, fence, value);
  }
  void Drain() {
    if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.Drain();
    else legacy_.Drain();
  }
  void Stop() { recorder_.Stop(); legacy_.Stop(); }
 private:
  Fm4LegacySubmitThread legacy_;
  Fm4RecorderSubmitThread recorder_;
};
]=])
fm4_recorder_replace("${fm4_new_class}"
    "${fm4_legacy_named}\n${fm4_new_named}\n${fm4_queue_facade}" "isolated legacy queue")
file(WRITE "${fm4_recorder_path}" "${fm4_recorder_text}")
message(STATUS "FM4 async recording applied")
