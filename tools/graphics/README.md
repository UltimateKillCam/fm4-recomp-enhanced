# Graphics diagnostics

Modified 2026-10-23.

These tools support renderer development. Build the enhanced SDK first, then run
`tools/Build-Graphics.cmd` from the repository root to build the D3D12 tools and
run resolve/input regressions. The tools require a Windows C++ toolchain and
the locally installed enhanced SDK.

Core targets cover resolve layout, register updates, camera recovery, scene
depth and diagnostic input. Optional DLSS, FSR and neural replay targets are
built only when their separate SDK headers are installed under the paths in
[CMakeLists.txt](CMakeLists.txt). Replay tools require compatible prepared frame
inputs supplied separately; passing a replay does not establish live gameplay
quality or performance.

For hidden renderer diagnostics, use `Run-Enhanced.ps1` with `-Background`,
`-TraceGraphics`, `-CaptureRovDepth`, `-CaptureOutput` and `-RunSeconds <limit>`.
Use `capture.request` for final output, `resolve.request` for ROV/depth data,
and `temporal-capture.request` for a same-frame temporal input/output pair in
the capture directory. Background mode mutes audio and suppresses physical
input feedback. Keep game-derived captures and vendor dependencies outside
source releases.

See [graphics settings and limitations](../../docs/ENHANCEMENTS.md),
[Neural Rendering](../../docs/NEURAL_RENDERING.md) and
[third-party notices](../../THIRD_PARTY_NOTICES.md).
