[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$seed=Join-Path $root '.local/rexglue-sdk'
$target=Join-Path $root '.local/rexglue-enhanced-sdk'
if(-not(Test-Path -LiteralPath (Join-Path $seed '.git'))){throw 'Build the pinned local SDK first with tools/Build-Local.ps1.'}
if(-not(Test-Path -LiteralPath $target)){
    & robocopy $seed $target /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /XJ /MT:8 /NFL /NDL /NP
    if($LASTEXITCODE -ge 8){throw 'Isolated SDK copy failed'}
}
$revision=& git -C $target rev-parse HEAD
if($LASTEXITCODE -ne 0 -or $revision -ne 'f16992c92e610c2d71773994436b442d9946f4e1'){throw 'Enhanced patch requires the pinned SDK revision.'}
$patch=Join-Path $root 'tools/graphics/rexglue-enhanced.patch'
& git -C $target apply --reverse --check $patch 2>$null
if($LASTEXITCODE -ne 0){
    & git -C $target apply --check $patch
    if($LASTEXITCODE -ne 0){throw 'SDK has conflicting local changes; patch was not applied.'}
    & git -C $target apply $patch
    if($LASTEXITCODE -ne 0){throw 'SDK patch application failed'}
}
Copy-Item -LiteralPath (Join-Path $root 'tools/graphics/texture_mods.h') -Destination (Join-Path $target 'include/rex/graphics/d3d12/fm4_texture_mods.h') -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/graphics/native_texture_upload.h') -Destination (Join-Path $target 'include/rex/graphics/d3d12/fm4_native_texture_upload.h') -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/graphics/diagnostic_input.h') -Destination (Join-Path $target 'include/rex/input/fm4_diagnostic_input.h') -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/graphics/resolve_capture.h') -Destination (Join-Path $target 'include/rex/graphics/d3d12/fm4_resolve_capture.h') -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/graphics/scene_depth.h') -Destination (Join-Path $target 'include/rex/graphics/d3d12/fm4_scene_depth.h') -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/graphics/frame_metrics.h') -Destination (Join-Path $target 'include/rex/graphics/d3d12/fm4_frame_metrics.h') -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/graphics/register_update.h') -Destination (Join-Path $target 'include/rex/graphics/d3d12/fm4_register_update.h') -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/sdk/temporal_bridge.h') -Destination (Join-Path $target 'include/rex/graphics/d3d12/fm4_temporal_bridge.h') -Force
Write-Host 'Enhanced SDK source is ready in its isolated folder.'
