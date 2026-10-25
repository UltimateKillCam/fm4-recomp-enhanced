# Graphics diagnostics

Modified 2026-10-23.

Build the enhanced SDK first, then run `tools/Build-Graphics.cmd` from the
repository root to build the optional Neural Rendering bridge. It requires a
Windows C++ toolchain and the NGX SDK headers at the path in
[CMakeLists.txt](CMakeLists.txt).

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
