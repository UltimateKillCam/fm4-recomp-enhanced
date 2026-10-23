[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GameDataRoot,
    [ValidateRange(1,64)][int]$Jobs=2,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$')][string]$WorkspaceName='clean-enhanced-game',
    [switch]$ValidateOnly
)

. (Join-Path $PSScriptRoot 'Release.Common.ps1')
$root=Get-Fm4RepositoryRoot
$policy=Get-Fm4ReleasePolicy
& (Join-Path $PSScriptRoot 'Test-RepositoryBoundary.ps1')
$game=Assert-Fm4GameInputs -GameDataRoot $GameDataRoot
$git=Resolve-Fm4Tool 'git'
$cmake=Resolve-Fm4Tool 'cmake'
$clang=Resolve-Fm4Tool 'clang'
$clangxx=Resolve-Fm4Tool 'clang++'
if((& $clangxx -dumpmachine).Trim() -notmatch 'windows-msvc$') { throw 'Use the validated MSVC-ABI Clang toolchain.' }
$sdkSource=Join-Path $root '.local/rexglue-enhanced-sdk'
$sdkInstall=Join-Path $root '.local/sdk-enhanced-install'
$sdkHead=(& $git -C $sdkSource rev-parse HEAD).Trim()
if($LASTEXITCODE -or $sdkHead -ne $policy.sdk.commit){throw 'Prepare the pinned enhanced SDK before building the game.'}
$patch=Join-Path $root 'tools/graphics/rexglue-enhanced.patch'
Invoke-CheckedNative $git @('-C',$sdkSource,'apply','--reverse','--check',$patch) $root
$sdkInputs=@('bin/rexglue.exe','bin/rexruntime.dll','bin/rexgpu-xenos.dll','lib/rexruntime.lib')
foreach($name in $sdkInputs){if(-not(Test-Path -LiteralPath (Join-Path $sdkInstall $name))){throw "Build the enhanced SDK first; missing $name"}}
$workspace=Join-Path (Join-Path $root '.local') $WorkspaceName
if(Test-Path -LiteralPath $workspace){throw "A clean build requires a new workspace name; already exists: $workspace"}
if($ValidateOnly){Write-Host 'Enhanced SDK, game inputs, compiler and fresh workspace validated; no compilation started.';return}

[IO.Directory]::CreateDirectory($workspace)|Out-Null
$work=Join-Path $workspace 'work'
$allowlist=@(Get-Content -LiteralPath (Join-Path $root $policy.source_allowlist) |
    ForEach-Object {$_.Trim().Replace('\','/')} | Where-Object {$_ -and -not $_.StartsWith('#')})
foreach($relative in $allowlist){
    $target=Join-Path $work $relative
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target))|Out-Null
    Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $target
}
$sourceRecords=Get-Fm4FileRecords -Paths @($allowlist|ForEach-Object {Join-Path $work $_}) -BasePath $work
$sdkRecords=Get-Fm4FileRecords -Paths @($sdkInputs|ForEach-Object {Join-Path $sdkInstall $_}) -BasePath $sdkInstall
$rexglue=Join-Path $sdkInstall 'bin/rexglue.exe'
$bootstrap=Join-Path $workspace 'bootstrap'
Invoke-CheckedNative $rexglue @('init','--project-name','fm4','--xex-path',(Join-Path $game 'default.xex'),'--game-root',$game,'--project-root',$bootstrap) $root
$project=Join-Path $work 'fm4'
[IO.Directory]::CreateDirectory((Join-Path $project 'generated'))|Out-Null
Copy-Item -LiteralPath (Join-Path $bootstrap 'generated/rexglue.cmake') -Destination (Join-Path $project 'generated/rexglue.cmake')
$localManifest=Join-Path $project 'fm4_manifest.local.toml'
$tomlRoot=$game.Replace('\','/').Replace('"','\"')
$manifest=(Get-Content -LiteralPath (Join-Path $project 'fm4_manifest.toml') -Raw).Replace('../extracted',$tomlRoot)
Write-Fm4Utf8File -Path $localManifest -Text $manifest
Invoke-CheckedNative $rexglue @('codegen',$localManifest) $project
Invoke-CheckedNative $cmake @('--preset','win-amd64-release',"-DCMAKE_PREFIX_PATH=$sdkInstall",
    "-Drexglue_DIR=$(Join-Path $sdkInstall 'lib/cmake/rexglue')",'-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
    "-DCMAKE_C_COMPILER=$clang","-DCMAKE_CXX_COMPILER=$clangxx") $project
Invoke-CheckedNative $cmake @('--build','--preset','win-amd64-release','--parallel',"$Jobs") $project
$output=Join-Path $project 'out/build/win-amd64-release'
Copy-Item -LiteralPath (Join-Path $project 'fm4_runtime.toml') -Destination (Join-Path $output 'fm4.toml')
$artifacts=@('fm4.exe','fm4_SpeechFacade_default.dll','fm4_XMediaFacade_default.dll','rexruntime.dll','rexgpu-xenos.dll','TracyClient.dll','fm4.toml') |
    ForEach-Object {Join-Path $output $_} | Where-Object {Test-Path -LiteralPath $_}
$receipt=[ordered]@{
    schema_version=1;created_utc=[DateTime]::UtcNow.ToString('o');generated_locally=$true
    fresh_translation=$true;reused_game_objects=$false;rebuilt_sdk_in_this_run=$false
    sdk_commit=$sdkHead;sdk_patch_sha256=(Get-FileHash -LiteralPath $patch).Hash
    sdk_artifacts=$sdkRecords;source_snapshot=$sourceRecords
    artifacts=(Get-Fm4FileRecords -Paths $artifacts -BasePath $output)
}
$receipt|ConvertTo-Json -Depth 9|Write-Fm4Utf8File -Path (Join-Path $workspace 'clean-build-provenance.json')
Write-Host "Fresh enhanced game build complete: $output"
