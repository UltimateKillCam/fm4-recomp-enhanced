# Experimental Neural Rendering

Modified 2026-10-23. GPL-3.0-only.

This optional pass invokes NVIDIA NGX feature 18 through a separately supplied
runtime before DLSS/FSR reconstruction. It is disabled by default and is not an
NVIDIA-supported FM4 integration. It has reached live driving on an RTX 4080;
that does not establish sustained stability, image quality or every option combination.

## Setup

1. Build the enhanced SDK and game using [Build notes](../BUILD-NOTES.md).
2. Supply the NGX SDK separately under `.local/streamline-current/external/ngx-sdk`.
   Run `tools/Build-Graphics.cmd` to build the source compatibility bridge
   `nvngx.dll_fm4.dll` under `.local/graphics-build`.
3. Supply compatible `nvngx_dlssnr.dll` and `nvngx_dlss.dll` in
   `.local/neural-runtime`, or select another runtime folder in the launcher.
4. Enable **Neural Rendering** in **FM4 Launcher.cmd**, save, and launch your build.

No vendor SDK, runtime or model weights are included. Vendor components retain
their own terms. The source compatibility forwarder is MIT-licensed and credited
in [UPSTREAM.md](../tools/neural/UPSTREAM.md); the host integration is GPL-3.0-only.
This project grants no permission to redistribute modified proprietary binaries.

## Settings

| Setting | Effect |
| --- | --- |
| Compatibility / `neural_path` | Automatic chooses by GPU generation; official and community force a path. |
| Runtime folder / `neural_runtime_folder` | Folder containing compatible external runtime DLLs; relative paths resolve from the repository root. |
| Passes / `neural_passes` | One to three chained features; more passes increase GPU cost. |
| Strength / `neural_strength` | 0..1 blend of the final processed result with the original input. |
| Model preset / `neural_preset` | Automatic (0), or preset 1..3 passed to the runtime. |
| Style / `neural_style` | Standard (0), Natural (1), or Cinematic (2), passed to the runtime. |

Processing occurs at render input resolution before reconstruction. HUD
protection uses a heuristic for bright neutral glyphs in common anchor regions;
complete HUD separation is unfinished.

```powershell
.\tools\Run-PowerShell.cmd -File .\tools\Run-Enhanced.ps1 -Profile 1440p -Upscaler DLSS -NeuralRendering -NeuralPasses 1 -NeuralStrength 0.7
```

Add `-WorkspaceName` for a named game build. `-NeuralPath auto|official|community`
and `-NeuralRuntimeFolder <folder>` override saved values for one launch.

## Runtime paths and limitations

| GPU | Automatic path | Boundary |
| --- | --- | --- |
| RTX 50 (Blackwell) | Official NVIDIA runtime | Implemented but untested on RTX 50 hardware. |
| RTX 40 (Ada) | Community compatibility runtime | Evaluated on RTX 4080; runtime architecture compatibility matters. |
| RTX 20/30 | Community path with warning | Untested; requires a compatible build. |
| GTX, AMD, Intel | Unavailable | Launcher refuses Neural Rendering. |

The official path requests feature 18 through NGX. If driver creation fails,
the host can use the source-built direct bridge with the supplied runtime.
The community path requires that bridge. A runtime filename alone does not
establish compatibility with the selected GPU.

The evaluated community NR runtime is 310.8.0, separate from Super Resolution
310.9.16. GPU timeouts and black output occurred in earlier configurations;
driver 617.42 improved bounded RTX 4080 checks but does not establish support
on other hardware. Successful driver-managed creation on RTX 50, complete
object motion, temporal quality and long-session stability remain unverified.
The launcher does not install drivers or replace system DLLs.
