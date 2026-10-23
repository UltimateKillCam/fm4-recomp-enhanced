# Forza Motorsport 4 Recompiled — Enhanced Edition

Modified 2026-10-23. Based on [Alexbeav/fm4-recomp](https://github.com/Alexbeav/fm4-recomp).

The Enhanced edition adds configurable graphics, optional user-supplied texture
packs, and wheel and controller support.
This is an experimental Windows port of the Xbox 360 game using
[ReXGlue](https://github.com/rexglue/rexglue-sdk).

**No game files, textures or binaries are included.** Supply an extracted copy
of your own legally obtained game and [build locally](BUILD-NOTES.md#build-and-launch).
Generated game translation and saves are also excluded.

![Forza Motorsport 4 Recompiled hero artwork](assets/fm4-recompiled-hero.png)

## Requirements

- Windows AMD64 with a D3D12-capable GPU.
- Git, CMake 3.25 or newer, Ninja, and Clang targeting the Windows MSVC ABI.
  LLVM 22 is the validated compiler; the SDK and game must use the same ABI.
- Extracted game data containing `default.xex`, `XMediaFacade_default.xex`
  and `SpeechFacade_default.xex`.
- Python 3.11 or newer for shader preparation and texture-pack tooling.
- Visual Studio C++ build tools and the Windows SDK for the enhanced SDK build.

The builder obtains the pinned [SDK fork](https://github.com/Alexbeav/rexglue-sdk/commit/f16992c92e610c2d71773994436b442d9946f4e1).
See [Build notes](BUILD-NOTES.md) for commands and toolchain constraints.

## Launch and configure

After building, open **FM4 Launcher.cmd**. **Save** remembers settings;
**Save and play** saves and launches. **Play Enhanced.cmd**
launches the default build with saved settings. For a fresh named build, use
**Save** and then the [workspace launch command](BUILD-NOTES.md#build-and-launch).
The shortcuts expect game data under
`.local/game-disc1`; command-line launches accept another game folder.

- [Wheel and controller settings](docs/WHEEL_SUPPORT.md): DirectInput wheels,
  separate pedals and shifters, controller switching, and DualSense feedback.
- [Graphics settings and limitations](docs/ENHANCEMENTS.md): native resolution, supersampling and experimental DLSS/FSR.
- [Texture packs](docs/TEXTURE_PACKS.md): import and load a pack you supply.
- [Experimental Neural Rendering](docs/NEURAL_RENDERING.md): requires a separate compatible runtime.
- [Project Forza Plus 4](docs/FORZA_PLUS.md): optional mod support using your own
  extracted mod, a separate prepared game copy and separate saves; off by default.

## Status and limitations

The game can boot, load profiles, complete races, save career progress and
install the four Disc 2 content packages. Career progression is verified through
Amateur and Clubman into Sportsman; later progression is unverified.
Blank car thumbnails, intermittent career-menu visibility and frame-pacing issues
remain possible. Physical wheel force feel and compatibility across wheel models
are not fully verified. Temporal graphics modes remain experimental.
See [Current status](docs/STATUS.md) for the supported boundary.

## License and notices

This derivative retains the original **GPL-3.0-only** license and notices.
The license covers only material the project is entitled to license; it grants
no rights to game files or generated game translation. See
[Legal](docs/LEGAL.md), [third-party notices](THIRD_PARTY_NOTICES.md),
[release notes](docs/RELEASE_NOTES.md) and [source release process](docs/RELEASING.md).

This project is not affiliated with or endorsed by Microsoft, Xbox, Turn 10
Studios, or the Forza franchise.
