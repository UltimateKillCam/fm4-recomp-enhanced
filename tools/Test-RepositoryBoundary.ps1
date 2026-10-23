# Modified 2026-10-18. Inspect tracked and allowlisted files so new release inputs receive the same checks.
[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot "Release.Common.ps1")

$root = Get-Fm4RepositoryRoot
$policy = Get-Fm4ReleasePolicy
$allowlistPath = Join-Path $root $policy.source_allowlist
$allowlist = @(Get-Content -LiteralPath $allowlistPath | ForEach-Object { $_.Trim().Replace("\", "/") } | Where-Object { $_ -and -not $_.StartsWith("#") })

$duplicates = @($allowlist | Group-Object | Where-Object Count -gt 1)
if ($duplicates) {
    throw "Duplicate source allowlist entries: $($duplicates.Name -join ', ')"
}

foreach ($relativePath in $allowlist) {
    if ([IO.Path]::IsPathRooted($relativePath) -or $relativePath.Split("/") -contains "..") {
        throw "Unsafe source allowlist path: $relativePath"
    }
    $fullPath = [IO.Path]::GetFullPath((Join-Path $root $relativePath))
    if (-not $fullPath.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Source allowlist path escapes the repository: $relativePath"
    }
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Source allowlist file is missing: $relativePath"
    }
    if ((Get-Item -LiteralPath $fullPath).Length -gt [int64]$policy.maximum_source_file_bytes) {
        throw "Source file exceeds the policy size limit: $relativePath"
    }
}

$allowed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($relativePath in $allowlist) { [void]$allowed.Add($relativePath) }
$gitMetadata = Join-Path $root ".git"
if (Test-Path -LiteralPath $gitMetadata) {
    $trackedPaths = @(& git -C $root ls-files)
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed"
    }
    $inspectionLabel = "tracked and allowlisted files"
    $inspectionSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $inspectionList = [Collections.Generic.List[string]]::new()
    foreach ($path in $trackedPaths) {
        if ($inspectionSet.Add($path)) { $inspectionList.Add($path) }
    }
    foreach ($path in $allowlist) {
        if ($inspectionSet.Add($path)) { $inspectionList.Add($path) }
    }
    $inspectedPaths = $inspectionList.ToArray()
}
else {
    [void]$allowed.Add("SOURCE-PROVENANCE.json")
    $inspectedPaths = @(Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object {
        (Get-Fm4RelativePath -BasePath $root -Path $_.FullName).Replace("\", "/")
    })
    $inspectionLabel = "source-package files"
}

$forbiddenExtensions = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($extension in $policy.forbidden_extensions) { [void]$forbiddenExtensions.Add($extension) }

foreach ($trackedPathRaw in $inspectedPaths) {
    $trackedPath = $trackedPathRaw.Replace("\", "/")
    if (-not $allowed.Contains($trackedPath)) {
        throw "Inspected file is absent from the source allowlist: $trackedPath"
    }
    foreach ($directory in $policy.forbidden_directories) {
        $prefix = $directory.TrimEnd("/") + "/"
        if ($trackedPath.Equals($directory, [StringComparison]::OrdinalIgnoreCase) -or
            $trackedPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Inspected file crosses a forbidden directory boundary: $trackedPath"
        }
    }
    if ($forbiddenExtensions.Contains([IO.Path]::GetExtension($trackedPath))) {
        throw "Inspected file has a forbidden extension: $trackedPath"
    }

    $fullPath = Join-Path $root $trackedPath
    if ((Get-Item -LiteralPath $fullPath).Length -ge 4) {
        $stream = [IO.File]::OpenRead($fullPath)
        try {
            $bytes = [byte[]]::new(4)
            [void]$stream.Read($bytes, 0, 4)
            $hex = [BitConverter]::ToString($bytes).Replace('-', '')
            $ascii = [Text.Encoding]::ASCII.GetString($bytes)
            if ($hex.StartsWith("4D5A") -or $ascii -in @("XEX2", "CON ", "LIVE", "PIRS")) {
                throw "Inspected file has forbidden binary magic: $trackedPath"
            }
        }
        finally {
            $stream.Dispose()
        }
    }
}

Write-Host "Repository boundary passed: $($inspectedPaths.Count) $inspectionLabel, $($allowlist.Count) allowlisted release files."
