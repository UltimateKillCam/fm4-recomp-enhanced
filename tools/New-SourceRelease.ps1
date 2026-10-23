# Modified 2026-10-18. Provenance output stays byte-stable on Windows PowerShell 5.1 and PowerShell 7.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern("^[0-9A-Za-z][0-9A-Za-z.-]*$")][string]$Version,
    [string]$OutputDirectory,
    [switch]$AllowDirty
)

. (Join-Path $PSScriptRoot "Release.Common.ps1")

$root = Get-Fm4RepositoryRoot
$policy = Get-Fm4ReleasePolicy
& (Join-Path $PSScriptRoot "Test-RepositoryBoundary.ps1")

$git = Resolve-Fm4Tool -Name "git"
$status = @(& $git -C $root status --porcelain)
if ($LASTEXITCODE -ne 0) { throw "git status failed" }
if ($status.Count -ne 0 -and -not $AllowDirty) {
    throw "The worktree is dirty. Commit the reviewed source or use -AllowDirty for a local review candidate."
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $root "release-out"
}
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$baseName = "fm4-recomp-source-$Version"
$zipPath = Join-Path $outputRoot "$baseName.zip"
$checksumPath = "$zipPath.sha256"
if ((Test-Path -LiteralPath $zipPath) -or (Test-Path -LiteralPath $checksumPath)) {
    throw "Release output already exists: $zipPath"
}

$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temporaryRoot = Join-Path $temporaryBase ("fm4-source-" + [Guid]::NewGuid().ToString("N"))
$packageRoot = Join-Path $temporaryRoot $baseName
[IO.Directory]::CreateDirectory($packageRoot) | Out-Null

try {
    $allowlistPath = Join-Path $root $policy.source_allowlist
    $allowlist = @(Get-Content -LiteralPath $allowlistPath | ForEach-Object { $_.Trim().Replace("\", "/") } | Where-Object { $_ -and -not $_.StartsWith("#") })
    foreach ($relativePath in $allowlist) {
        $source = Join-Path $root $relativePath
        $destination = Join-Path $packageRoot $relativePath
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination
    }

    $projectCommit = (& $git -C $root rev-parse HEAD).Trim()
    $repository = (& $git -C $root remote get-url origin).Trim()
    $payloadRecords = @($allowlist | ForEach-Object {
        $file = Join-Path $packageRoot $_
        [ordered]@{
            path = $_
            size = (Get-Item -LiteralPath $file).Length
            sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
        }
    })
    $provenance = [ordered]@{
        schema_version = 1
        package = $baseName
        project_repository = $repository
        project_commit = $projectCommit
        project_worktree_dirty = ($status.Count -ne 0)
        distribution_model = $policy.distribution_model
        project_license = $policy.project_license
        sdk_repository = $policy.sdk.repository
        sdk_commit = $policy.sdk.commit
        payload = $payloadRecords
    }
    $provenanceJson = ConvertTo-Json -InputObject $provenance -Depth 8 -Compress
    Write-Fm4Utf8File -Path (Join-Path $packageRoot "SOURCE-PROVENANCE.json") -Text ($provenanceJson + "`n")

    if (-not ("Fm4.Release.SourceArchive" -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot "SourceArchive.cs")
    }
    [Fm4.Release.SourceArchive]::Create($temporaryRoot, $zipPath)

    $zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    "$zipHash  $([IO.Path]::GetFileName($zipPath))" | Set-Content -LiteralPath $checksumPath -Encoding ascii
    Write-Host "Source review candidate created: $zipPath"
    Write-Host "SHA-256: $zipHash"
}
finally {
    $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
    if ($resolvedTemporaryRoot.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedTemporaryRoot).StartsWith("fm4-source-")) {
        Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
