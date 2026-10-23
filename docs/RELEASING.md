# Source release process

Modified 2026-10-23.

Public artifacts contain reviewed source and checksums. Publication requires the
project owner’s approval of the exact contents and workflow.

## Contents

The source allowlist is [release/source-files.txt](../release/source-files.txt).
[release/release-policy.json](../release/release-policy.json) records policy,
pinned SDK identity and required game inputs.

Exclude game files, disc images, content packages, Xbox system files, textures,
generated game translation, executables, DLLs, SDK binaries, object files,
debug symbols, saves, caches, captures and local build/dependency directories.
Retain the project license and all third-party notices.

## Package and review

From a clean reviewed checkout:

```powershell
.\tools\Run-PowerShell.cmd -File .\tools\Test-RepositoryBoundary.ps1
.\tools\Run-PowerShell.cmd -File .\tools\New-SourceRelease.ps1 -Version 0.1.0-alpha.1
```

The packager writes a source ZIP, a SHA-256 checksum and an embedded
`SOURCE-PROVENANCE.json`. Inspect archive entries against the allowlist and
confirm the source and SDK commits and a clean worktree. Repeat packaging in
separate output directories with the same PowerShell runtime and compare hashes;
compression can differ between PowerShell 5.1 and 7. A dirty candidate is not
publishable.

Before release, validate game inputs, complete a clean local build and check
startup, saves, race completion, content installation and the edition’s input
features. Report unresolved limitations from [STATUS.md](STATUS.md). Submit the
archive entry list, checksums and release text for owner approval.

Users build locally as described in [Build notes](../BUILD-NOTES.md). The build
scripts download pinned SDK sources and generate translation on the user’s
machine; game inputs and generated translation are not uploaded.
