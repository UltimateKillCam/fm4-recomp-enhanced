# Importing and loading texture packs

Modified 2026-10-23.

No texture pixels or packs are included. Import or load a pack you supply;
keep it under the ignored `textures` directory or outside the repository.
The original game archives are not rewritten. Missing or invalid replacements
fall back to original game resources.

## Load a pack

Open **FM4 Launcher.cmd**, choose **Textures**, select a folder or import a pack
ZIP, then save. A folder may contain DDS files directly or in a `replacements`
subdirectory. Restart the game after switching packs.

| Texture pack mode | Behavior |
| --- | --- |
| Automatic | Uses the pack only if `manifest.json` says `"complete": true`. Default folder: `textures/Enhanced`. |
| On | Uses the selected folder regardless of completion status. |
| Off | Uses original textures. |

Older `enable_textures` true/false settings map to On/Off. Command-line
`-TexturePack` and `-NoTextures` override the saved setting for that launch:

```powershell
.\tools\Run-PowerShell.cmd -File .\tools\Run-Enhanced.ps1 -TexturePack "D:\Textures\MyPack"
.\tools\Run-PowerShell.cmd -File .\tools\Run-Enhanced.ps1 -NoTextures
```

For a named build workspace, add `-WorkspaceName` as described in
[Build notes](../BUILD-NOTES.md#build-and-launch).

## Pack utility

Use Python 3.11 or newer with [tools/Texture-Packs.py](../tools/Texture-Packs.py):

```powershell
python tools/Texture-Packs.py check "D:\Textures\MyPack"
python tools/Texture-Packs.py import "D:\Downloads\MyPack.zip" --name MyPack
python tools/Texture-Packs.py status textures/MyPack
python tools/Texture-Packs.py finish textures/MyPack
python tools/Texture-Packs.py combine textures/PackA textures/PackB --name Combined
python tools/Texture-Packs.py export textures/MyPack --output "D:\Exports\MyPack.zip"
python tools/Texture-Packs.py repair textures/MyPack textures/MyPack-fixed
```

Import creates `textures/MyPack/replacements`, validates the DDS files and
marks the pack complete. Existing packs are not overwritten. ZIP imports reject
scripts, executables, traversal paths, duplicate identities and symlinks.
`finish` validates every file and requires the file set and hashes to match
the manifest before marking it complete. `combine` creates one finished pack
and refuses conflicting replacements for the same identity.

Export writes validated DDS files, a manifest and a separate SHA-256 checksum;
it does not upload anything or include source images, saves or machine paths.

`repair` writes a new pack. Where a BC3 replacement's top level is transparent, it restores the colour
from the original level below, removing coloured fringes and surfaces that some upscalers leave behind.
It needs numpy; other levels and alpha are kept unchanged.

## Compatibility and limitations

DDS names encode the original format, dimensions and content hash; replacements
are matched by identity. Packs must retain the expected format and uniform 2x
or 4x dimensions, include a complete mip chain and stay within the 4096-pixel
dimension limit. RGBA replacements additionally require **Load uncompressed textures**.
The game caps pack memory at 512 MB.

Structural validation cannot establish correct UV layout, material masks,
transparency or appearance. A pack can load successfully and still cause visual
artifacts. Disable it to compare against the original game resources.
Game-derived packs are outside the source release and source-code license;
see [Legal](LEGAL.md).
