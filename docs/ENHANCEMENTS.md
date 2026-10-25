# Enhanced graphics settings

Modified 2026-10-23. GPL-3.0-only.

The Enhanced edition adds internal 720p, 1440p and 4K rendering, supersampling,
optional external texture packs and experimental temporal processing. Open
**FM4 Launcher.cmd** to configure settings. Changes apply on the next launch;
the launcher does not change desktop resolution.

## Rendering

| Setting | Effect |
| --- | --- |
| Output resolution | Native 1280 x 720, 2560 x 1440, or 3840 x 2160 presentation. |
| Native | Draws at output resolution; uses FM4's multisample anti-aliasing. Saved as `upscaler=Bilinear`, `internal_scale=0`. |
| Supersampling | Draws one profile size above output and downsamples with an area filter. Not offered at 4K output; costs more GPU time. |
| DLSS / FSR | Renders at the selected lower resolution, then reconstructs to output size. Requires separately installed vendor components and compatible hardware. |
| Native + CAS | Native rendering with contrast-adaptive sharpening. |
| Native + FSR 1 | Legacy spatial scaling/sharpening path. |
| Render resolution | Input size for temporal modes or Neural Rendering; Automatic uses the mode's default. The window shows render and output sizes. |
| GPU adapter | Selects the named adapter; empty uses automatic selection. |
| Frame rate limit | Sets the emulated display refresh (60-240). Races use measured time, so game speed stays correct above 60; menus run at half the limit. 120 suits 1440p; 240 suits native 4K. Above 60 is experimental. |

DLSS integration targets Streamline 2.14.16 / DLSS 310.9.16; FSR targets the
FidelityFX SDK 2.3.0 signed API. Provider selection depends on hardware.
Vendor SDKs and runtime DLLs are separate dependencies; see their upstream
[Streamline](https://github.com/NVIDIA-RTX/Streamline),
[DLSS](https://github.com/NVIDIA/DLSS) and
[FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) projects.

Temporal modes use scene depth, camera motion, jitter and history rejection.
Independent object motion and complete HUD separation are unfinished, so moving
objects, menus and text can show artifacts. Native DLAA is not offered. Frame
generation is not offered.

## Textures and content

- **Texture pack** selects Automatic, On or Off. Import and load a pack you supply;
  see [Texture packs](TEXTURE_PACKS.md).
- **Load uncompressed textures** enables RGBA replacements; this uses more memory.
- **Distant texture detail** applies mip bias: Sharp 0, Balanced 0.5, Soft 1.0.
  Sharper mip choices can shimmer; softer choices reduce distant detail.
- **DLC folder** accepts extracted content-package folders named by a 16- or
  42-digit content ID. Launch links them into the profile's content directory;
  the runtime can mount headerless extracted packages. Mounted content appears
  as `Content opened:` in the game log. Supply your own content.

## Advanced options

Keep the supplied defaults unless diagnosing a specific issue. These settings
change renderer synchronization or compatibility and need broader validation.

| Setting / saved key | Effect |
| --- | --- |
| Batch graphics copies / `readback_batch` | Retains full host copies while batching completion work. |
| Finish graphics copies on GPU / `d3d12_readback_resolve_gpu_completion` | Orders guest completion markers behind GPU copies in the batching path. |
| Optimise CPU command processing / `gpu_bulk_type0_register_writes` | Uses bulk register writes where their side effects can be preserved. |
| Visibility fallback value / `query_occlusion_fake_sample_count` | Synthetic sample count when real occlusion results are unavailable; can affect visibility and exposure. |
| Combine visibility results / `gpu_occlusion_query_cumulative_fallback` | Uses cumulative synthetic counter snapshots. |
| Sample scaled readbacks at pixel centres / `readback_center_sample` | Changes the sample offset used to read scaled resolves. |
| Experimental host render targets / `host_render_targets` | Selects the RTV path instead of ROV. |
| Use readback heap instead of host copies / `readback_heap` | Disables direct host-copy readback. |
| Submit each GPU completion separately / `d3d12_readback_resolve_completion_boundary` | Flushes at each completion marker; can add submission overhead. |
| Submit at every command-buffer end / `d3d12_submit_on_primary_buffer_end` | Adds submission boundaries at primary-buffer completion. |

Performance overrides (added 2026-10-24, set under `runtime_overrides`, all default off):

| Override | Effect |
|---|---|
| `d3d12_persistent_pipeline_cache` | Keeps translated shaders and pipelines on disk between runs, reducing first-use stutter. |
| `d3d12_texture_range_reuse` | Reloads only changed texture ranges and reuses unchanged converted textures. |
| `d3d12_resolve_readback_barrier_batch` | Removes duplicate GPU barriers around render-target copies. |
| `host_render_targets_depth_scissor_ownership` | Limits depth-only render-target copies to the drawn area. |
| `host_render_targets_scissor_ownership` | Limits all render-target copies to the area a draw can touch. |
| `host_render_targets_msaa_samples_max` = 1 with `host_render_targets_msaa_collapse` | Renders the game's MSAA surfaces single-sampled when resolution scaling is on; about 15-20% less GPU time at 4K, but shows rectangles in the mirrors on some cars and tracks. Not recommended. |
Native render targets (`gpu_native_render_targets`, default on, requires the
RTV path): replaces the Xbox 360 EDRAM emulation with one D3D12 surface per
render target, native clears and MSAA, and converts aliased surfaces only when
their contents are read. Cleared contents are recreated with a native clear
instead of being copied. In paired race tests it cut GPU time per frame by
about 33% at 1440p and 41% at native 4K, with identical images (mirrors, HUD,
shadows). Set it to `false` under `runtime_overrides` to return to EDRAM
emulation. The game's banded (tiled) passes are still drawn band by band; GPU
trace snapshots are not supported on this path.

Other scalar runtime overrides are stored under `runtime_overrides` in
`.local/enhancements/settings.json`. Defaults come from
[config/enhanced-settings.json](../config/enhanced-settings.json) and
[fm4_runtime.toml](../fm4/fm4_runtime.toml).

## Build and limitations

Follow [Build notes](../BUILD-NOTES.md#build-and-launch) to build the enhanced SDK
and game. [Neural Rendering](NEURAL_RENDERING.md) is an independent optional pass,
disabled by default. Wheel/controller setup is covered in [Wheel support](WHEEL_SUPPORT.md).

High internal resolution and extra passes increase GPU and memory cost. Missing
geometry, replacement-material correctness, showroom reflections, thumbnails and
frame pacing still need wider car/track coverage. A successful load does not
establish correct appearance in every scene. See [Current status](STATUS.md).
