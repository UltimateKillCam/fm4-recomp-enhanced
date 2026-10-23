// Copyright (c) 2026 xilla. MIT licensed; see licenses/dlss5-sidecar-mit.txt.
// Copied from Randolin/dlss5-wow-sidecar-revised, source attribution in UPSTREAM.md.
// The contract between wowsidecar.exe and nvngx.dll_sidecar.dll.
//
// Shared by both sides, and by nothing else. Plain C so the forwarder stays a
// C-ABI library with no dependency on the sidecar's C++ headers, and so the
// host resolves every entry point by name with GetProcAddress rather than
// linking an import library that would tie the two binaries' versions together
// silently. sidecar_nr_abi_version() is the check that they agree.
//
// NGX results are returned as int: 1 is NVSDK_NGX_Result_Success, anything
// with the high bit set is an NVSDK_NGX_Result failure code, and the small
// negative numbers below are the forwarder's own -- distinguishable from NGX's
// because NGX never returns a value in that range.
#pragma once

#define SIDECAR_NR_ABI_VERSION 1

// The forwarder's own failures, distinct from NGX result codes.
#define SIDECAR_NR_ERR_RUNTIME_NOT_LOADED (-1)
#define SIDECAR_NR_ERR_BAD_ARGUMENT (-2)
#define SIDECAR_NR_ERR_NOT_INITIALISED (-3)

#ifdef __cplusplus
extern "C" {
#endif

// The model's tuning. Read when the feature is created; a change means a new
// feature. Ranges are what the model's own vocabulary allows: preset 0-3 where
// 0 leaves the choice to the model; style 0 standard, 1 natural, 2 cinematic;
// the four strengths 0-2 with skin structure allowing -1 for "off".
typedef struct SidecarNrTuning {
  unsigned int preset;
  unsigned int style;
  float intensity;
  float localStructure;
  float localTone;
  float skinStructure;
  unsigned int useAutoMask;
  unsigned int uiCorrection;
} SidecarNrTuning;

// One evaluation. Colour and output are the working resolution; depth and
// motion are the guide resolution, which for this sidecar is the same size.
// Resources are ID3D12Resource* passed as void* so this header needs no D3D.
typedef struct SidecarNrEval {
  void* color;
  void* depth;
  void* motion;
  void* output;
  unsigned int width;
  unsigned int height;
  unsigned int guideWidth;
  unsigned int guideHeight;
  unsigned int depthInverted;
  unsigned int reset;
  float mvScaleX;
  float mvScaleY;
} SidecarNrEval;

#ifdef __cplusplus
}
#endif
