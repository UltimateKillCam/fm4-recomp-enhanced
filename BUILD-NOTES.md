# FM4 build notes

Modified 2026-10-23.

## Build and launch

Run commands from the repository root. Replace `D:\Games\FM4-extracted`
with your extracted game folder. Windows launchers find PowerShell 7 or fall
back to Windows PowerShell 5.1; no PATH change is needed for launch or settings.

Build the enhanced SDK first, then generate and build the game in a fresh workspace:

```powershell
.\tools\Build-Enhanced-Sdk.cmd
.\tools\Run-PowerShell.cmd -File .\tools\Build-Enhanced-Game.ps1 -GameDataRoot "D:\Games\FM4-extracted" -Jobs 2
.\tools\Run-PowerShell.cmd -File .\tools\Run-Enhanced.ps1 -GameDataRoot "D:\Games\FM4-extracted" -WorkspaceName clean-enhanced-game
```

`Build-Enhanced-Sdk.cmd` prepares the pinned SDK and resolve shaders, then builds
and installs the enhanced runtime under `.local/sdk-enhanced-install`.
`Build-Enhanced-Game.ps1` uses `.local/clean-enhanced-game` by default; choose
a new `-WorkspaceName` for another clean build. Pass that name to
`Run-Enhanced.ps1` when launching that build.

Add `-ValidateOnly` to the build command to check its prerequisites without
compilation (the enhanced SDK must already be prepared and built).
Local dependencies, generated translation and build outputs stay under `.local`.
Profiles, saves and caches use `.local/user`.

For folder shortcuts, place your extracted Disc 1 data under `.local/game-disc1`.
The shortcuts expect `.local/work/fm4/out/build/win-amd64-release`. For the fresh
enhanced build, use **Save** in the settings window, then launch with the command
above and `-WorkspaceName clean-enhanced-game`; shortcuts do not select that
workspace automatically.

To install Disc 2 content, extract the four content packages into a folder and
pass it as `-Disc2Root` to `Run-Enhanced.ps1`.
Folder shortcuts use `.local/disc2-content`. Installed packages are kept in
user data and do not require the source folder on later starts.

## Pinned Windows toolchain

Build the SDK and FM4 with the same C++ ABI. The validated packaging build uses
LLVM 22's MSVC target (`C:\Program Files\LLVM\bin\clang++.exe`) for both. The
Retcomm compiler targets MinGW/libc++; it must not consume an SDK built for the
MSVC ABI because exported C++ symbols have different names and standard-library
types.

The pinned SDK needs a feature-gated compatibility definition of
`std::chrono::clock_time_conversion` and `std::chrono::clock_cast` when
`__cpp_lib_chrono` is older than C++20. Its libmspack build must also use the
canonical `libmspack/libmspack/mspack` source directory. The cabextract path is
a symbolic-link mirror that may be checked out as one-line text files on
Windows. These fixes are recorded in SDK commit
[`f16992c`](https://github.com/Alexbeav/rexglue-sdk/commit/f16992c92e610c2d71773994436b442d9946f4e1).

The baseline Build-Local path installs the SDK under `.local\sdk-install` and
builds FM4 under:

```text
.local\work\fm4\out\build\win-amd64-release
```

It asks the SDK CLI to generate its standard CMake bootstrap locally, copies
only allowlisted project source into `.local\work`, and generates all
game-derived translation there.

## Generated function registrar

FM4 generates approximately 292,000 function registrations in one C++ source
file. Clang 22 exhausts memory when it optimizes the original single function.
Compiling the original function at `-O0` can leave late registrations unusable
at runtime.

The CMake build runs the splitter automatically when code generation changes
the registrar. To apply it manually for inspection, run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Split FM4 Registrar.ps1"
```

The idempotent script rewrites `fm4/generated/default/fm4_register.cpp` into
bounded, non-inlined helper functions. `fm4/CMakeLists.txt` compiles this
generated file and the startup-only `fm4_init.cpp` dispatcher at `-O0`. The
dispatcher otherwise exhausts memory during an optimized compile on a busy
development host. Recompiled game-code translation units keep the release
optimization settings.

SDK configurations that precompile the generated `fm4_pch.h` must exclude the
two `-O0` startup sources. Clang rejects a PCH built at the release optimization
level when the consuming translation unit uses `-O0`.

Before testing a rebuilt executable, verify that the script reports the same
registration count before and after splitting.

The main executable dispatch table is populated from `PPCFuncMappings` in
`fm4/generated/default/fm4_init.cpp`. A targeted manual rebuild after adding a
function must compile that file, `fm4_register.cpp`, and every affected
`fm4_recomp.*.cpp` segment before relinking. `fm4_register.cpp` alone does not
update the main-image dispatch table.

When the manifest's dynamic-module set changes, also rebuild
`generated/default/module_registry.cpp`. Relinking with a stale registry can
silently omit a facade DLL and fail only when the game first loads that module.
