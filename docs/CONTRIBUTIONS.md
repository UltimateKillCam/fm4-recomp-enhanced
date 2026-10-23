# Builder and upstream contribution notes

Modified 2026-10-23.

Keep FM4-specific data in this project and submit general runtime fixes to
ReXGlue as focused patches with tests. Useful areas include Xenon vector
semantics, empty resolve handling, dynamic-module unload cleanup, writable cache
devices, XAM message `0x2B003`, indirect-dispatch continuations and bounded
generated function registration.

FM4 requires full resolve readback for its correctness profile. Faster readback
heuristics need dependency tracking or a title-safe fallback. Signed-integer
resolve packing and texture-fetch scaling should be assessed separately from
the title’s visibility workarounds. Keep Disc 2 discovery policy here; generic
STFS validation and content management belong in the SDK.

Recompilation metadata includes title ID `4D530910`, the main executable and
two facade modules, function tables and module paths. Rebuild the module registry
when module membership changes; see [Build notes](../BUILD-NOTES.md).

Preserve copyright, license and dated modification notices. Do not contribute
game executables, extracted assets, profiles, generated game translation or
content packages. See [Legal](LEGAL.md) and [release policy](RELEASING.md).
