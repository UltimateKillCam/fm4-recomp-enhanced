# Third-party software notices

The source archive references and locally builds the following dependency.
The win-x64 player ZIP includes its runtime DLLs and retains license texts
under `licenses`:

- [ReXGlue SDK](https://github.com/Alexbeav/rexglue-sdk), pinned by
  `release/release-policy.json`, is licensed under the BSD 3-Clause License.
  It includes or links additional components under their own terms and
  contains code derived from [Xenia](https://github.com/xenia-project/xenia),
  also under the BSD 3-Clause License.

The pinned SDK checkout is the authority for the complete dependency list and
the license texts applicable to a local build. Anyone who redistributes SDK
files or locally produced binaries must preserve the notices and satisfy the
licenses shipped with that exact SDK source. The FM4 source packager does not
package those files. The player packager includes runtime DLLs, their notices,
and app-local Microsoft Visual C++ redistributable DLLs from the compiler installation.
Microsoft runtime source is not part of this project; those DLLs retain Microsoft's redistribution terms.

`tools/sdk/Runtime-Fixes.cmake` contains adapted ReXGlue/Xenia source fragments
for build-local runtime overlays, modified 2026-10-17. Their original notices
remain in the generated sources; the BSD notice is retained in
[licenses/rexglue-xenia-bsd.txt](licenses/rexglue-xenia-bsd.txt).

`tools/sdk/Gpu-Completion.cmake` adapts cumulative synthetic visibility-report
semantics from [Xenia Canary](https://github.com/xenia-canary/xenia-canary/tree/50e9c715df05e3d93ac18e253d14956900683226),
particularly `pm4_command_processor_implement.h` and `xenos_zpd_report.h`.
Copyright 2026 Ben Vanik. All rights reserved. The BSD 3-Clause terms are
retained in [licenses/rexglue-xenia-bsd.txt](licenses/rexglue-xenia-bsd.txt).

## Enhanced graphics source

The enhanced SDK patch modifies BSD-licensed ReXGlue/Xenia code. The complete
BSD notice is retained in [licenses/rexglue-xenia-bsd.txt](licenses/rexglue-xenia-bsd.txt).
Original per-file copyright notices remain in the patch. Shader source inputs
are pinned by commit and SHA-256 in `tools/graphics/resolve-shaders-lock.json`;
`tools/Prepare-Resolve-Shaders.py` preserves their notices while adapting the
pinned runtime ABI. These modifications are dated 2026-10-16.

NVIDIA Streamline/DLSS, AMD FidelityFX SDKs, Microsoft's texture-conversion
tools are separate local downloads under their respective upstream terms. They are not included in this source repository or
its source archives. Game-derived texture packs are also excluded and are not
covered by the source-code license. The texture importer/exporter does not
upload images.

The source-only Neural Rendering compatibility forwarder comes from
[Randolin/dlss5-wow-sidecar-revised](https://github.com/Randolin/dlss5-wow-sidecar-revised)
at the commit recorded in [tools/neural/UPSTREAM.md](tools/neural/UPSTREAM.md).
Its [MIT license](licenses/dlss5-sidecar-mit.txt) is retained. The separate
NVIDIA NGX SDK and proprietary neural runtime, including private community
compatibility builds, are not part of either public source edition. This
project grants no permission to redistribute a modified NVIDIA binary.

## Project artwork

The repository includes the user-supplied project image
`assets/fm4-recompiled-hero.png`. Names, logos, vehicle designs, and other
third-party visual elements in that image are not offered under the project's
GPL license. All such rights remain with their respective owners. The project
is not affiliated with or endorsed by Microsoft, Xbox, Turn 10 Studios,
Ferrari, or the Forza franchise.
