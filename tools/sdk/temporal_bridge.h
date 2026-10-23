#pragma once
// One temporal-upscaler owner inside rexruntime; GPU and UI DLLs share it.
// GPL-3.0-only, 2026-10-16.
#include <cstdint>
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
#ifdef FM4_TEMPORAL_BRIDGE_EXPORT
#define FM4_TEMPORAL_API __declspec(dllexport)
#else
#define FM4_TEMPORAL_API __declspec(dllimport)
#endif
extern "C" {
FM4_TEMPORAL_API bool Fm4TemporalPreInitialize();
FM4_TEMPORAL_API bool Fm4TemporalDevice(ID3D12Device* device);
FM4_TEMPORAL_API bool Fm4TemporalOutputSize(uint32_t width,uint32_t height,uint32_t* output_width,uint32_t* output_height);
FM4_TEMPORAL_API void Fm4TemporalUpgradeSwapChain(void** swapchain);
FM4_TEMPORAL_API ID3D12Resource* Fm4TemporalInput(ID3D12Device* device,uint32_t width,uint32_t height);
FM4_TEMPORAL_API bool Fm4TemporalInputSizeChanged(uint32_t width,uint32_t height);
FM4_TEMPORAL_API void Fm4TemporalObserveDraw(const float* constants,const float* track_constants,uint32_t surface,uint32_t depth,const float* viewport);
FM4_TEMPORAL_API void Fm4TemporalJitter(uint32_t surface,uint32_t depth,const float* viewport,uint32_t host_width,uint32_t host_height,float* ndc_x,float* ndc_y);
FM4_TEMPORAL_API void Fm4TemporalQueue(ID3D12Resource* input,ID3D12Resource* output,ID3D12Resource* depth,bool valid_depth);
FM4_TEMPORAL_API bool Fm4TemporalHasQueuedFrame();
FM4_TEMPORAL_API bool Fm4TemporalUsesNeuralQueue();
FM4_TEMPORAL_API void Fm4TemporalExecuteNeuralQueue(ID3D12Device* device,ID3D12CommandQueue* guest_queue,uint64_t completed_submission,uint64_t submission);
FM4_TEMPORAL_API void Fm4TemporalExecute(ID3D12Device* device,ID3D12GraphicsCommandList* list,uint64_t completed_submission,uint64_t submission);
FM4_TEMPORAL_API void Fm4TemporalNextFrame();
FM4_TEMPORAL_API void Fm4TemporalShutdownGpu();
FM4_TEMPORAL_API void Fm4TemporalShutdown();
FM4_TEMPORAL_API void Fm4TemporalDeviceLostDiagnostic(ID3D12Device* device);
}
