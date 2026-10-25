# Optional mods

Mod support is experimental and off by default. It requires a player built with
`FM4_ENABLE_MODS=ON` and an SDK built with `FM4_MODS=ON`. The normal launcher and
existing settings keep their current behavior. No enabled mods, no mods folder,
or an omitted `--enable` selects stock paths. Restart after changing mods.

Use Python 3.11 or newer. Keep mods and prepared data outside the extracted game:

```text
mods/<id>/mod.toml
mods/<id>/files/...
mods/<id>/patches/...
mods/<id>/pack/replacements/*.dds
mods/<id>/extension.dll
```

Only `mod.toml` has a fixed name; content paths are explicit. For example:

```toml
id = "example"
name = "Example changes"
version = "1.0.0"
enabled = true
order = 10
game_version = "b34dfb88978819cc" # SHA-256 prefix of the original default.xex
gameplay = true

[files]
"media/example.txt" = "files/example.txt"

[[archives]]
path = "media/example.zip"
member = "example.txt"
source = "files/member.txt"

[[database]]
path = "media/db/gamedb.slt"
patch = "patches/parts.json"
```

`id` must match the directory and use lowercase letters, digits, `_` or `-`.
`name`, `version`, and `game_version` are required. `enabled` defaults to false;
`order` defaults to zero. Higher order wins conflicting file/member/texture
identities; ties sort by id. Database operations run in that order.
Later complete-file replacements also discard earlier member, row and text
edits to that file. An invalid
enabled mod rejects preparation. Version prefixes contain 16–64 hexadecimal
digits. `"*"` is available for fixtures and version-independent content; native
guest helpers still verify the supported executable and instructions.

Prepare without starting the game:

```powershell
python tools/mods/prepare.py prepare --enable --game D:/FM4/Disc1 --mods mods --state .local/mods --stock-user .local/user
python tools/mods/prepare.py verify <printed-session.json-path>
```

Launch a matching locally built player:

```powershell
python tools/mods/launch.py --enable --exe <player-folder>/fm4.exe --game D:/FM4/Disc1 --mods mods --state .local/mods --stock-user .local/user -- --gpu_plugin=xenos
```

The tool passes `FM4_MODS=1` and `FM4_MOD_SESSION=<receipt>` only to that child.
For an existing private launch harness, set those two values in its child
environment after preparing and verifying the receipt. Keep its game and stock
user paths equal to the preparation arguments. The live launcher has no Mods
page yet; this workflow is intended for local builds and integration testing.
Do not enable mods with an older player/runtime pair.

Preparation makes a new content-keyed session under `state/sessions/<hash>`.
Only changed files are copied. The runtime checks hashes and holds read leases
on prepared files before mounting a read-only VFS overlay. It never rewrites
the extracted game, archives, executable or mod inputs. Replaced protected files
receive recalculated checks in a separate guest-memory resource before the
title starts. The original native FM4 executable is required; title-update
mounts and simultaneous Forza Plus prepared sessions are rejected.

Loose overrides replace exact game-relative paths. Archives are rebuilt as
ZIP32, including ZIP-format `.pak` files. Existing member names must match;
untouched members retain their compressed bytes, including FM4 LZX members.
Unknown container formats, ZIP64, encrypted archives, links, duplicate member
names and traversal paths are rejected. Added loose files can be opened by
exact path; directory enumeration continues to show the original game tree.

Database patch files contain JSON arrays of updates, with no SQL commands:

```json
[
  {"table":"List_PartsStrings","where":{"Id":1},"set":{"PartsString":"Example component"},"expect":1}
]
```

Each operation needs a nonempty `where`, a `set`, and an exact positive expected
row count. Values bind as parameters. Failure rolls back the copied database;
no completed session is published. Integrity and foreign-key checks must pass.
Schema changes, insertion and deletion are outside this initial patch format.

Mods containing native code, database patches, SQLite text edits, loose overrides or archive
overrides always receive an empty separate profile under
`state/profiles/<session-hash>`. `gameplay=true` also requests isolation.
Textures and replacement text alone can share the configured stock profile.
Turning mods off returns to stock saves. Changing the enabled set or its content
selects another profile. DLC installations are not automatically copied into
the new profile; install the owner's content there through the normal importer.

For textures, add `textures = "pack"` to the manifest. Packs use the existing
[texture-pack format](TEXTURE_PACKS.md). Preparation validates and merges them
into one pack for the existing loader; later mods win. Pass `--base-texture`
to preserve a separately selected pack underneath mod packs. Without that
argument, mod textures replace the selected pack for that session. The existing
format, dimension, mip, RGBA opt-in and memory limits still apply.

Replacement text uses the actual string-table id, not its row position:

```toml
[[strings]]
archive = "media/stringtables/en.zip"
table = "AIDiff.str"
id = 43014
text = "EXAMPLE DIFFICULTY"
```

The preparer rebuilds the UTF-16 text pool of a checked LSB2 table and preserves
its key sections. Unknown layouts and missing ids fail. For LZX-compressed
tables, add `--codec-runtime <mod-capable-player-folder>/rexruntime.dll`; this
uses the SDK decoder and verifies the decompressed ZIP CRC before editing.
Other members keep their exact compressed payload. Localized SQLite text can
instead bind `path`, `table`, `id_column`, `id`, `text_column`, and `text`.

Native extensions use [fm4_mod.h](../tools/mods/fm4_mod.h) and export
`Fm4ModStart`. Add `native = "extension.dll"` and optional string-valued
`[settings]`. DLLs run with the player's permissions: use trusted code. Only
the plugin directory and Windows system directory are searched for dependencies;
the initial format stages one self-contained plugin DLL per mod.

The ABI uses a major/minor version and structure sizes, fixed-width values,
UTF-8 text, and opaque host handles. The host table remains valid until unload.
Frame callbacks run on the overlay UI thread once per overlay draw; update
callbacks run before the title's race update on its guest thread, including
multiple updates per rendered frame. Menu actions run on the next UI draw.
All callbacks serialize. Keep them short, never retain guest pointers, and do
not call the API after unload or from plugin-owned background threads.

Guest byte reads validate the whole span (maximum 1 MiB). Writes require an
update callback, committed writable pages, and an address outside the executable
module. Bytes retain guest endian order. Faulting copies return an error.
`call` accepts only the curated `presentation.set_interval` helper with two
32-bit arguments: presentation object address and interval 0–3. It verifies the
setter instructions and object write span, runs only inside an update callback,
and restores the complete guest register context. Unknown symbols return an
error. This initial helper surface is intentionally small.

HUD coordinates use a 1280×720 reference canvas and scale with the window.
Each plugin owns its label and action ids; empty labels remove them. Open
**F8** for mod actions, or **F6 → Mods** when the live settings overlay is built.
The menu captures guest keyboard/controller input. Settings persist separately
in the selected user profile's `mod-settings.json` with backups.

`replace_string(table,id,text)` queues an existing string binding owned by the
calling mod and returns `FM4_MOD_RESTART`. Feed the selected profile's
`mod-string-requests.json` to the next preparation with `--string-requests`.
Text takes effect on that next launch; already-loaded string tables are not
hot patched. The declarative `[[strings]]` form needs no native DLL.

Independent examples live under [tools/mod-samples](../tools/mod-samples).
The checked-in manifests are disabled; copy a sample, enable it and build
`native/example.c` as a Windows x64 DLL for the native sample.
Never distribute prepared game files, extracted resources or game-derived packs.
