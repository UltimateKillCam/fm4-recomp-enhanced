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

| Frame rate limit | Sets the emulated display refresh (60-240), or Unlimited, which presents as fast as the PC allows while physics keeps the console step rate. Races use measured time, so game speed stays correct above 60; menus run at half the limit. 120 suits 1440p; 240 suits native 4K. Above 60 is experimental. |



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

| `gpu_native_render_targets` | Experimental logical D3D12 surfaces, native MSAA/clears, lazy format aliases and GPU resolves. Default off; full-scene band collapse remains disabled pending clear-initialization validation, and wider scene/performance coverage is required. GPU trace snapshots are unsupported on this path. |
| `d3d12_recorder_thread` | Records D3D12 command lists on worker threads, taking work off the GPU command thread. |
| `d3d12_binding_layout_compare`, `d3d12_constant_blocks`, `d3d12_native_alias_exact_range`, `d3d12_state_domain_batch`, `d3d12_ticketed_submit_ordering` | CPU fast paths for draw-state, constant and submission handling. Requires an SDK built with `-DFM4_CPU_QUICKWINS=ON`. |
| `fm4_streaming_asset_pool_mb` | Guest streaming asset pool, 132-160 MB, default 160. Lower values can leave roadside textures blurred at high frame rates. |
| `n2_pm4_fast` | Processes guest GPU register and constant ranges with native AVX2 copies and compares instead of per-word handlers. Needs `-DFM4_CPU_QUICKWINS=ON`. |
| `n3_async_queue` | Moves remaining queue submissions off the GPU command thread onto the ordered submit worker. Use with `d3d12_async_submit` and `d3d12_recorder_thread`; keep `d3d12_submit_draw_interval` at 256. Needs `-DFM4_CPU_QUICKWINS=ON`. |
| `n5_draw_reuse` | Reuses texture preparation and descriptor validation between draws while their textures are unchanged. |
| `n1_hotpath_cache` | Caches readback policy lookups and diagnostic switches on the command thread; recorder timing then needs `FM4_RECORDER_TIMING=1`. |
| `n4_native_hot`, `n4_native_transform` | Runs the game's byte fill/copy and vector transform helpers as native code, with exact results. |
| `m1_native_more` | Runs six more of the game's math, table-decode and search helpers as native code, with exact results. |
| `m2_draw_core` | Caches pipeline and root-signature selection, render-target setup and unchanged system constants between draws. |
| `m3_present_lean` | Stops redrawing the hidden live-settings overlay every frame and wakes present-driven vblank by event instead of polling. |
| `m4_overlap` | Experimental: earlier command-ring consumption and event-driven register waits. Leave off unless testing. |




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


## In-game graphics settings (experimental)

Configure the SDK with `-DFM4_LIVE_SETTINGS=ON` and rebuild the SDK and game.
With that matching build, set `runtime_overrides.fm4_live_settings` to
`true` before launching. Press **F6**, or **Back + Start** on a controller, for
Display, Rendering and Performance pages. Use the D-pad and A to select controls.
**Apply and save** changes spatial scaling, internal 1x–3x resolution, window
size/mode, FXAA, mip bias, anisotropy and the host frame cap between frames.
Rebuilding rendering caches may pause briefly. Launch with Unlimited to use the
full range of live caps; a session's guest refresh remains fixed.

Changes merge into `.local/enhancements/settings.json`, with a backup kept beside
it. Window dimensions are saved separately from internal scale; borderless
fullscreen uses the desktop mode. Temporal DLSS/FSR and texture-pack enablement
are labelled **Apply on next launch**. Temporal sessions require a restart for
rendering changes. This feature is off by default pending wider scene coverage.
