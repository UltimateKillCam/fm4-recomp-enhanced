[CmdletBinding()]
param(
    [ValidateSet('Native','1440p','4K')][string]$Profile='Native',
    [ValidateSet('Bilinear','FSR1','CAS','DLSS','FSR')][string]$Upscaler='Bilinear',
    [switch]$NeuralRendering,
    [ValidateRange(1,3)][int]$NeuralPasses=1,
    [ValidateRange(0,3)][int]$NeuralPreset=0,
    [ValidateRange(0,2)][int]$NeuralStyle=0,
    [ValidateRange(0.0,1.0)][double]$NeuralStrength=1.0,
    [string]$NeuralRuntimeFolder='',
    [ValidateSet('auto','official','community')][string]$NeuralPath='auto',
    [string]$SettingsFile='',
    [string]$RuntimeConfig='',
    [switch]$SelfTestTexturePackSettings,
    [ValidateRange(0,7)][int]$InternalScale=0,
    [string]$AdapterName='',
    [string]$GameDataRoot='',
    [string]$UserDataRoot='',
    [string]$Disc2Root='',
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$')][string]$WorkspaceName,
    [string]$SdkInstallRoot='',
    [switch]$ValidateOnly,
    [switch]$SelfTestSdkCopies,
    [switch]$DumpTextures,
    [string]$TextureDumpFolder='',
    [switch]$NoTextures,
    [string]$TexturePack='',
    [switch]$EnableRgbaTextures,
    [switch]$Metrics,
    [switch]$ExperimentalReadbackBatch,
    [switch]$HostRenderTargets,
    [switch]$ReadbackHeap,
    [switch]$ReadbackCenterSample,
    [switch]$TraceInput,
    [switch]$TraceGraphics,
    [switch]$CaptureOutput,
    [switch]$CaptureRovDepth,
    [switch]$DiagnosticDred,
    [switch]$Background,
    [string]$DiagnosticInput='',
    [ValidateRange(0,3)][int]$DiagnosticDevices=3,
    [ValidateRange(0,3600)][int]$RunSeconds=0
)
$ErrorActionPreference='Stop'
# Closing the game window hard-exits the runtime after it has stopped the title and
# flushed its log; a shared DLL then fast-fails in the C runtime during process teardown
# (0xC0000409). That is a completed close, not a failed launch.
function Test-Fm4CleanClose([int]$ExitCode,[string]$Log){
    if($ExitCode -ne -1073740791 -or -not(Test-Path -LiteralPath $Log -PathType Leaf)){return $false}
    $tail=@(Get-Content -LiteralPath $Log -Tail 6 -ErrorAction SilentlyContinue)
    return [bool]($tail|Where-Object{$_ -like '*Title terminated; hard-exiting process.*'})
}
. (Join-Path $PSScriptRoot 'Release.Common.ps1')
. (Join-Path $PSScriptRoot 'Neural.Common.ps1')
$root=Get-Fm4RepositoryRoot
function Copy-Fm4FileIfDifferent([string]$Source,[string]$Destination,[switch]$ValidateOnly) {
    if(-not(Test-Path -LiteralPath $Source -PathType Leaf)){throw "Required file missing: $Source"}
    if($ValidateOnly){return $false}
    $copy= -not(Test-Path -LiteralPath $Destination -PathType Leaf)
    if(-not $copy){$copy=(Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash}
    if($copy){
        $parent=Split-Path -Parent $Destination
        if($parent -and -not(Test-Path -LiteralPath $parent -PathType Container)){New-Item -ItemType Directory -Path $parent -Force|Out-Null}
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
    }
    return $copy
}
if($SelfTestSdkCopies){
    $testRoot=Join-Path ([IO.Path]::GetTempPath()) ('fm4-sdk-copy-selftest-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $testRoot -Force|Out-Null
    try {
        $source=Join-Path $testRoot 'source.dll';$destination=Join-Path $testRoot 'install/bin/runtime.dll'
        [IO.File]::WriteAllBytes($source,[Text.Encoding]::ASCII.GetBytes('source-version-one'))
        if(-not(Copy-Fm4FileIfDifferent $source $destination)){throw 'Missing destination was not installed.'}
        if([IO.File]::ReadAllText($destination) -ne 'source-version-one'){throw 'Initial installation copied incorrect bytes.'}
        $before=[IO.File]::GetLastWriteTimeUtc($destination)
        if(Copy-Fm4FileIfDifferent $source $destination){throw 'Equal destination was copied unnecessarily.'}
        if([IO.File]::GetLastWriteTimeUtc($destination) -ne $before){throw 'Equal destination timestamp changed.'}
        [IO.File]::WriteAllBytes($source,[Text.Encoding]::ASCII.GetBytes('source-version-two'))
        if(-not(Copy-Fm4FileIfDifferent $source $destination)){throw 'Different destination was not updated.'}
        if([IO.File]::ReadAllText($destination) -ne 'source-version-two'){throw 'Different destination was not replaced.'}
        $validateOnlyPath=Join-Path $testRoot 'validate-only/missing.dll'
        if(Copy-Fm4FileIfDifferent -Source $source -Destination $validateOnlyPath -ValidateOnly:$true){throw 'ValidateOnly reported a copy.'}
        if(Test-Path -LiteralPath $validateOnlyPath){throw 'ValidateOnly created a destination.'}
        Write-Output 'PASS: SDK copy installs missing files, skips equal hashes, replaces changed files, and leaves destinations untouched in ValidateOnly.'
    } finally {
        $resolved=[IO.Path]::GetFullPath($testRoot)
        $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
        if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing SDK-copy self-test cleanup outside the temporary directory.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    }
    return
}
if(-not $SettingsFile){
    $SettingsFile=Join-Path $root '.local/enhancements/settings.json'
    if(-not(Test-Path -LiteralPath $SettingsFile)){$SettingsFile=Join-Path $root 'config/enhanced-settings.json'}
}
$settings=Get-Content -LiteralPath $SettingsFile -Raw -Encoding UTF8 | ConvertFrom-Fm4Json
$knownKeys=@('dlc_folder','forza_plus','profile','internal_scale','upscaler','frame_rate_limit','gpu_adapter_name','texture_pack','texture_mode','enable_textures','enable_rgba_textures','readback_batch','host_render_targets','readback_heap','readback_center_sample','runtime_overrides','neural_rendering','neural_passes','neural_preset','neural_style','neural_strength','neural_runtime_folder','neural_path')
foreach($key in $settings.Keys){if($key -notin $knownKeys){throw "Unknown enhanced setting: $key"}}
if($settings.ContainsKey('frame_rate_limit')){
    $FrameRateLimit=$settings.frame_rate_limit
    if(($FrameRateLimit -isnot [int] -and $FrameRateLimit -isnot [long]) -or $FrameRateLimit -notin @(60,120,144,165,240)){throw 'Frame rate limit must be 60, 120, 144, 165 or 240.'}
}else{$FrameRateLimit=60}
foreach($key in @('enable_textures','enable_rgba_textures','readback_batch','host_render_targets','readback_heap','readback_center_sample','neural_rendering')){
    if($settings.ContainsKey($key) -and $settings[$key] -isnot [bool]){throw "Setting $key must be true or false."}
}
if($settings.ContainsKey('neural_runtime_folder') -and $settings.neural_runtime_folder -isnot [string]){throw 'Neural Rendering folder must be text.'}
if(-not $PSBoundParameters.ContainsKey('NeuralPath') -and $settings.ContainsKey('neural_path')){
    if($settings.neural_path -isnot [string] -or $settings.neural_path -notin @('auto','official','community')){throw 'Neural Rendering compatibility must be auto, official or community.'}
    $NeuralPath=[string]$settings.neural_path
}
if(-not $PSBoundParameters.ContainsKey('Profile') -and $settings.profile){$Profile=[string]$settings.profile}
if($Profile -notin @('Native','1440p','4K')){throw 'Invalid settings profile.'}
if(-not $PSBoundParameters.ContainsKey('Upscaler') -and $settings.upscaler){$Upscaler=[string]$settings.upscaler}
if($Upscaler -notin @('Bilinear','FSR1','CAS','DLSS','FSR')){throw 'Unsupported upscaler.'}
$temporal=$Upscaler -in @('DLSS','FSR')
if(-not $PSBoundParameters.ContainsKey('NeuralRendering') -and $settings.ContainsKey('neural_rendering')){$NeuralRendering=$settings.neural_rendering}
foreach($entry in @(@('NeuralPasses','neural_passes',1,3),@('NeuralPreset','neural_preset',0,3),@('NeuralStyle','neural_style',0,2))){
    if(-not $PSBoundParameters.ContainsKey($entry[0]) -and $settings.ContainsKey($entry[1])){
        $value=$settings[$entry[1]]
        if(($value -isnot [int] -and $value -isnot [long]) -or $value -lt $entry[2] -or $value -gt $entry[3]){throw "Invalid $($entry[1]) setting."}
        Set-Variable -Name $entry[0] -Value ([int]$value)
    }
}
if(-not $PSBoundParameters.ContainsKey('NeuralStrength') -and $settings.ContainsKey('neural_strength')){
    $value=$settings.neural_strength
    if($value -isnot [ValueType] -or -not (Test-Fm4Finite ([double]$value)) -or $value -lt 0 -or $value -gt 1){throw 'Neural Rendering strength must be between 0 and 1.'}
    $NeuralStrength=[double]$value
}
$temporalPipeline=$temporal -or $NeuralRendering
if(-not $PSBoundParameters.ContainsKey('InternalScale') -and $settings.ContainsKey('internal_scale')){
    if($settings.internal_scale -isnot [long] -and $settings.internal_scale -isnot [int]){throw 'Render scale must be a whole number.'}
    $InternalScale=[int]$settings.internal_scale
}
if($InternalScale -lt 0 -or $InternalScale -gt 7){throw 'Render scale must be 0 to 7; 0 is automatic.'}
if(-not $PSBoundParameters.ContainsKey('AdapterName') -and $settings.gpu_adapter_name){$AdapterName=[string]$settings.gpu_adapter_name}
# Texture pack mode: auto uses the configured pack only once its manifest marks it finished
# ("complete": true, written by tools/Texture-Packs.py finish after validation), on always
# uses it and off keeps the original textures. Older settings map enable_textures to on/off.
function Get-TextureMode([hashtable]$values) {
    if($values.ContainsKey('texture_mode')){
        if($values.texture_mode -notin @('auto','on','off')){throw 'Texture pack mode must be auto, on or off.'}
        return [string]$values.texture_mode
    }
    if($values.ContainsKey('enable_textures')){return $(if($values.enable_textures){'on'}else{'off'})}
    return 'auto'
}
function Test-TexturePackFinished([string]$folder) {
    if(-not $folder){return $false}
    if((Split-Path -Leaf $folder) -eq 'replacements'){$folder=Split-Path -Parent $folder}
    $manifest=Join-Path $folder 'manifest.json'
    if(-not(Test-Path -LiteralPath $manifest -PathType Leaf)){return $false}
    try {$data=Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json} catch {return $false}
    $complete=$data.PSObject.Properties['complete']
    return ($null -ne $complete -and $complete.Value -eq $true)
}
$textureMode=Get-TextureMode $settings
$textureEnabled=$textureMode -ne 'off'
function Resolve-ConfiguredTexturePack([string]$configuredPath,[bool]$enabled,[string]$repositoryRoot,[bool]$skipConfiguredPack=$false) {
    if($skipConfiguredPack -or -not $enabled -or -not $configuredPath){return ''}
    $candidate=$configuredPath
    if(-not [IO.Path]::IsPathRooted($candidate)){$candidate=Join-Path $repositoryRoot $candidate}
    if(Test-Path -LiteralPath $candidate -PathType Container){return [IO.Path]::GetFullPath($candidate)}
    $defaultPack=[IO.Path]::GetFullPath((Join-Path $repositoryRoot 'textures/Enhanced')).TrimEnd('\')
    if([IO.Path]::GetFullPath($candidate).TrimEnd('\') -ne $defaultPack){
        throw "Texture pack folder not found: $candidate. Choose another folder or turn texture packs off."
    }
    return ''
}
if($SelfTestTexturePackSettings){
    $testRoot=Join-Path ([IO.Path]::GetTempPath()) ('fm4-texture-settings-selftest-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $testRoot -Force|Out-Null
    try {
        $default=Resolve-ConfiguredTexturePack 'textures/Enhanced' $true $testRoot
        if($default){throw 'Missing optional default pack should leave stock textures selected.'}
        $missing=Join-Path $testRoot 'missing-user-pack'
        try {Resolve-ConfiguredTexturePack $missing $true $testRoot;throw 'Missing custom pack was accepted.'}
        catch {if($_.Exception.Message -notmatch 'Configured texture pack folder does not exist'){throw}}
        if((Resolve-ConfiguredTexturePack $missing $false $testRoot)){throw 'Disabled texture setting did not ignore a stale path.'}
        if((Resolve-ConfiguredTexturePack $missing $true $testRoot $true)){throw '-NoTextures did not bypass a stale custom path.'}
        $partialSettings=@{texture_pack=$missing}
        $partialEnabled=if($partialSettings.ContainsKey('enable_textures')){[bool]$partialSettings.enable_textures}else{$true}
        try {Resolve-ConfiguredTexturePack $partialSettings.texture_pack $partialEnabled $testRoot;throw 'Partial settings stopped using the configured custom pack.'}
        catch {if($_.Exception.Message -notmatch 'Configured texture pack folder does not exist'){throw}}
        $existing=Join-Path $testRoot 'mine';New-Item -ItemType Directory -Path $existing|Out-Null
        if((Resolve-ConfiguredTexturePack $existing $true $testRoot) -ne [IO.Path]::GetFullPath($existing)){throw 'Existing custom pack path did not resolve.'}
        if((Get-TextureMode @{}) -ne 'auto'){throw 'Missing texture settings did not select auto.'}
        if((Get-TextureMode @{enable_textures=$false}) -ne 'off' -or (Get-TextureMode @{enable_textures=$true}) -ne 'on'){throw 'enable_textures did not map to off/on.'}
        if((Get-TextureMode @{texture_mode='auto';enable_textures=$false}) -ne 'auto'){throw 'texture_mode did not take precedence.'}
        try {Get-TextureMode @{texture_mode='sometimes'};throw 'Invalid texture_mode was accepted.'} catch {if($_.Exception.Message -notmatch 'Texture pack mode must be'){throw}}
        if(Test-TexturePackFinished $existing){throw 'A pack without a manifest counted as finished.'}
        Set-Content -LiteralPath (Join-Path $existing 'manifest.json') -Value '{"schema":1,"textures":[]}' -Encoding UTF8
        if(Test-TexturePackFinished (Join-Path $existing 'replacements')){throw 'An unfinished manifest counted as finished.'}
        Set-Content -LiteralPath (Join-Path $existing 'manifest.json') -Value '{"schema":1,"complete":true,"textures":[]}' -Encoding UTF8
        if(-not(Test-TexturePackFinished (Join-Path $existing 'replacements'))){throw 'A finished manifest was not recognised.'}
        Write-Output 'PASS: missing optional default, missing custom, disabled stale path, existing custom, texture modes and finished-pack detection.'
    } finally {
        $resolved=[IO.Path]::GetFullPath($testRoot)
        $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
        if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing self-test cleanup outside the temporary directory.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    }
    return
}
if(-not $PSBoundParameters.ContainsKey('NoTextures')){$NoTextures=$textureMode -eq 'off'}
if(-not $PSBoundParameters.ContainsKey('TexturePack')){
    $configuredPack=if($settings.texture_pack){[string]$settings.texture_pack}else{'textures/Enhanced'}
    $TexturePack=Resolve-ConfiguredTexturePack $configuredPack $textureEnabled $root ([bool]$NoTextures)
    $texturePackFinished=Test-TexturePackFinished $TexturePack
    if($textureMode -eq 'auto' -and -not $NoTextures){
        if($TexturePack -and $texturePackFinished){
            Write-Host "Texture pack: $TexturePack"
        } else {
            if($TexturePack){Write-Host "Pack unfinished; using original textures: $TexturePack"}
            $TexturePack='';$NoTextures=$true
        }
    }
} else {
    $texturePackFinished=Test-TexturePackFinished $TexturePack
}
if(-not $PSBoundParameters.ContainsKey('NeuralRuntimeFolder')){
    $NeuralRuntimeFolder=if($settings.ContainsKey('neural_runtime_folder') -and $settings.neural_runtime_folder){[string]$settings.neural_runtime_folder}else{'.local/neural-runtime'}
}
if(-not $NeuralRuntimeFolder.Trim()){throw 'Choose a Neural Rendering runtime folder.'}
if(-not [IO.Path]::IsPathRooted($NeuralRuntimeFolder)){$NeuralRuntimeFolder=Join-Path $root $NeuralRuntimeFolder}
$NeuralRuntimeFolder=[IO.Path]::GetFullPath($NeuralRuntimeFolder)
if(-not $PSBoundParameters.ContainsKey('EnableRgbaTextures') -and $settings.ContainsKey('enable_rgba_textures')){$EnableRgbaTextures=$settings.enable_rgba_textures}
# Batched GPU-ordered readback is the validated race-performance default (2026-10-18).
if(-not $PSBoundParameters.ContainsKey('ExperimentalReadbackBatch')){$ExperimentalReadbackBatch=if($settings.ContainsKey('readback_batch')){[bool]$settings.readback_batch}else{$true}}
if(-not $PSBoundParameters.ContainsKey('HostRenderTargets') -and $settings.ContainsKey('host_render_targets')){$HostRenderTargets=$settings.host_render_targets}
if(-not $PSBoundParameters.ContainsKey('ReadbackHeap') -and $settings.ContainsKey('readback_heap')){$ReadbackHeap=$settings.readback_heap}
if(-not $PSBoundParameters.ContainsKey('ReadbackCenterSample') -and $settings.ContainsKey('readback_center_sample')){$ReadbackCenterSample=$settings.readback_center_sample}
if($NoTextures -and -not $PSBoundParameters.ContainsKey('TexturePack')){$TexturePack=''}
if($NoTextures -and $TexturePack){throw 'Choose either -NoTextures or -TexturePack.'}
if($TextureDumpFolder -and -not $DumpTextures){throw 'TextureDumpFolder requires DumpTextures.'}
if($DiagnosticInput -and -not $Background){throw 'Diagnostic controller fixtures require -Background.'}
if($PSBoundParameters.ContainsKey('DiagnosticDevices') -and -not $DiagnosticInput){throw 'DiagnosticDevices requires DiagnosticInput.'}
if($CaptureOutput -and -not $Background){throw 'Renderer captures require -Background.'}
if($CaptureRovDepth -and (-not $Background -or -not $TraceGraphics)){throw 'ROV depth diagnostics require -Background -TraceGraphics.'}
if($DiagnosticDred -and (-not $Background -or -not $TraceGraphics)){throw 'GPU diagnostics require -Background -TraceGraphics.'}
$preservationPath=Join-Path $root '.local/enhancements/baseline-manifest.json'
if(Test-Path -LiteralPath $preservationPath){
    $preservation=Get-Content -LiteralPath $preservationPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if($root -eq $preservation.baseline){throw 'Enhanced launcher cannot run in the preserved original folder.'}
}
$workspace=Join-Path $root '.local'
if($WorkspaceName){$workspace=Join-Path $workspace $WorkspaceName}
$build=Join-Path $workspace 'work/fm4/out/build/win-amd64-release'
if(-not $SdkInstallRoot){$SdkInstallRoot=Join-Path $root '.local/sdk-enhanced-install'}
$SdkInstallRoot=[IO.Path]::GetFullPath($SdkInstallRoot)
$sdk=Join-Path $SdkInstallRoot 'bin'
# These three binaries are built together. Keep the profiler dependency paired
# with the runtime rather than retaining a potentially incompatible older copy.
foreach($dll in @('rexruntime.dll','rexgpu-xenos.dll','TracyClient.dll')){
    $source=Join-Path $sdk $dll
    if(-not(Test-Path -LiteralPath $source)){throw 'Enhanced graphics support is missing; run tools/Build-Enhanced-Sdk.cmd.'}
    $destination=Join-Path $build $dll
    Copy-Fm4FileIfDifferent -Source $source -Destination $destination -ValidateOnly:$ValidateOnly|Out-Null
}
if(-not $GameDataRoot){$GameDataRoot=Join-Path $root '.local/game-disc1'}
if(-not $UserDataRoot){$UserDataRoot=Join-Path $root '.local/user'}
$game=Assert-Fm4GameInputs -GameDataRoot $GameDataRoot
$UserDataRoot=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($UserDataRoot)
$scale=@{Native=1;'1440p'=2;'4K'=3}[$Profile]
$outputScale=$scale
if($temporal){$scale=1}
if($InternalScale){$scale=$InternalScale}
if($temporalPipeline -and $scale -gt $outputScale){throw 'Render resolution must not exceed output resolution.'}
if(-not $RuntimeConfig){$RuntimeConfig=Join-Path $root 'fm4/fm4_runtime.toml'}
$config=Get-Content -LiteralPath $RuntimeConfig -Raw -Encoding UTF8
function Set-RuntimeValue([string]$key,$value){
    if($key -notmatch '^[A-Za-z_][A-Za-z0-9_]*$'){throw "Invalid advanced setting name: $key"}
    if($value -is [bool]){$encoded=if($value){'true'}else{'false'}}
    elseif($value -is [string]){$encoded=ConvertTo-Json -InputObject $value -Compress}
    elseif($value -is [ValueType] -and $value -isnot [DateTime] -and (Test-Fm4Finite ([double]$value))){$encoded=[Convert]::ToString($value,[Globalization.CultureInfo]::InvariantCulture)}
    else {throw "Advanced setting $key must be a number, true/false or text."}
    $pattern='(?m)^'+[regex]::Escape($key)+'\s*=.*$'
    $line="$key = $encoded"
    if([regex]::IsMatch($script:config,$pattern)){$script:config=[regex]::Replace($script:config,$pattern,[Text.RegularExpressions.MatchEvaluator]{param($match) $line}.GetNewClosure())}
    else {$script:config+="`n$line`n"}
}
# Fewer, larger GPU submissions: each D3D12 submit that reaches an idle GPU can block for
# several milliseconds. ~56-59 vs ~36 race FPS with the same rendering on the tested tracks.
# Both remain overridable through runtime_overrides (Advanced tab).
Set-RuntimeValue 'video_mode_refresh_rate' $FrameRateLimit
Set-RuntimeValue 'vblank_frequency_override' $FrameRateLimit
Write-Host "Frame rate limit: $FrameRateLimit FPS"
Set-RuntimeValue 'd3d12_readback_resolve_completion_boundary' $false
Set-RuntimeValue 'd3d12_submit_on_primary_buffer_end' $false
# Higher resolutions sample sharper mips than FM4's 720p target; a small bias reduces
# tarmac and grass shimmer. Settings > Textures can change it (runtime_overrides).
Set-RuntimeValue 'texture_mip_bias' 0.5
if($settings.runtime_overrides){
    if($settings.runtime_overrides -isnot [Collections.IDictionary]){throw 'Invalid advanced graphics settings.'}
    foreach($key in $settings.runtime_overrides.Keys){
        if($key -in @('resolution_scale','log_file','d3d12_adapter_name','present_effect','audio_mute','fullscreen','headless','d3d12_readback_resolve_frame_batch','readback_resolve_half_pixel_offset')){throw "Set $key in the launcher."}
        Set-RuntimeValue $key $settings.runtime_overrides[$key]
    }
}
$runFolder=Join-Path $root '.local/enhancements/runs'
New-Item -ItemType Directory -Path $runFolder -Force | Out-Null
$runId="$(Get-Date -Format 'yyyyMMdd-HHmmss')-$Profile-$Upscaler"
$runLog=Join-Path $runFolder "$runId.log"
$config=$config -replace '(?m)^log_file\s*=.*$',('log_file = "'+($runLog -replace '\\','/')+'"')
$config=$config -replace '(?m)^resolution_scale\s*=.*$',"resolution_scale = $scale"
$config += "`n# Enhanced profile: $Profile; original simulation clock.`n"
Set-RuntimeValue 'd3d12_adapter_name' $AdapterName
$effect=@{Bilinear='bilinear';FSR1='fsr';CAS='cas';DLSS='bilinear';FSR='bilinear'}[$Upscaler]
Set-RuntimeValue 'present_effect' $effect
Set-RuntimeValue 'mnk_minimum_key_hold_ms' 80
Set-RuntimeValue 'd3d12_readback_resolve_frame_batch' ([bool]$ExperimentalReadbackBatch)
if($HostRenderTargets){$config=$config -replace '(?m)^render_target_path_d3d12\s*=.*$','render_target_path_d3d12 = "rtv"'}
if($ReadbackHeap){$config=$config -replace '(?m)^d3d12_readback_resolve_host_copy\s*=.*$','d3d12_readback_resolve_host_copy = false'}
Set-RuntimeValue 'readback_resolve_half_pixel_offset' ([bool]$ReadbackCenterSample)
if($TraceGraphics){Set-RuntimeValue 'render_target_lifecycle_trace' $true; Set-RuntimeValue 'render_target_lifecycle_trace_limit' 6000}
if($Background){
    Set-RuntimeValue 'fullscreen' $false
    Set-RuntimeValue 'audio_mute' $true
    Set-RuntimeValue 'mnk_mode' $false
    Set-RuntimeValue 'headless' $true
}
$neuralPlan=$null
if($NeuralRendering){
    $neuralPlan=Resolve-Fm4NeuralPath -Requested $NeuralPath -AdapterName $AdapterName
    Write-Host ('Neural Rendering: '+($neuralPlan.Summary -replace ' \((?:un)?tested[^)]*\)$',''))
    foreach($warning in $neuralPlan.Warnings){Write-Warning ($warning -replace ' is untested here; it needs a community runtime built for this GPU and costs much more GPU time\.',' needs a compatible community runtime and may run slowly.' -replace 'The official path is meant for RTX 50\. On other GPUs the driver is expected to decline; the direct bridge is then used if it is built\.','NVIDIA compatibility needs RTX 50; other cards may use the fallback.')}
    if($neuralPlan.Path -eq 'official'){Write-Warning 'RTX 50 Neural Rendering is experimental.'}
}
if($ValidateOnly){
    $preview=Join-Path $root '.local/enhancements/settings-preview.toml'
    Write-Fm4Utf8File -Path $preview -Text $config
    Write-Host "Settings valid: $([int]$scale*1280)x$([int]$scale*720); $Upscaler. Preview: $preview"
    if($neuralPlan -and -not $neuralPlan.Supported){throw ($neuralPlan.Warnings -join ' ')}
    return
}
Write-Fm4Utf8File -Path (Join-Path $build 'fm4.toml') -Text $config
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=Join-Path $build 'fm4.exe'; $start.WorkingDirectory=$build; $start.UseShellExecute=$false
$start.CreateNoWindow=$true
# Optional Project Forza Plus 4 (settings key forza_plus). Off keeps the stock route unchanged.
. (Join-Path $PSScriptRoot 'ForzaPlus.Common.ps1')
. (Join-Path $PSScriptRoot 'ForzaPlus.Launch.ps1')
$forzaPlus=$null;$forzaPlusState=$null
$pfpCandidate=Get-Pfp4Settings $settings
if($pfpCandidate.enabled -eq $true){
    $forzaPlus=$pfpCandidate
    $pfpBackend=Join-Path $root 'tools/forza_plus/backend_cli.py'
    $null=Get-Pfp4Decision -Settings $forzaPlus -EditionRoot $root
    $forzaPlusState=Invoke-Pfp4Backend -Operation inspect -EditionRoot $root -Settings $forzaPlus -BackendCliPath $pfpBackend -GameExecutable $start.FileName
    if($forzaPlusState.status -in @('prepared_missing','options_changed')){
        Write-Host 'Preparing Forza Plus; the first launch may take several minutes…'
        $forzaPlusState=Invoke-Pfp4Backend -Operation prepare -EditionRoot $root -Settings $forzaPlus -BackendCliPath $pfpBackend -GameExecutable $start.FileName
    }
    if($forzaPlusState.status -ne 'ready' -or -not $forzaPlusState.launch_allowed){throw "Project Forza Plus 4 is not ready: $($forzaPlusState.status)"}
    $commonWheel=Join-Path $UserDataRoot 'wheel.ini'
    if(Test-Path -LiteralPath $commonWheel -PathType Leaf){
        $null=Sync-Pfp4WheelSettings -CommonWheelIni $commonWheel -ModeUserRoot $forzaPlusState.user_data_root -Mode $forzaPlus.mode `
            -ReceiptPath (Join-Path $root ('.local/forza-plus/wheel-sync-'+$forzaPlus.mode+'.json')) -AdmissionLeaseHeld $true -GenerationReady $true
    }
    $game=[string]$forzaPlusState.game_data_root;$UserDataRoot=[string]$forzaPlusState.user_data_root
    Write-Host "Forza Plus mode: $($forzaPlus.mode)."
}
# Extracted DLC packages (added 2026-10-23): link every package folder from the dlc_folder
# setting (for example the Project Forza Plus 4 "Disc 2 and DLCs" folder) into the profile's
# marketplace content folder. The runtime mounts package folders without headers; nothing is
# copied. Forza Plus mode prepares its own content and is left alone.
if(-not $forzaPlus -and $settings.ContainsKey('dlc_folder') -and $settings.dlc_folder){
    $dlcRoot=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath([string]$settings.dlc_folder)
    if(-not(Test-Path -LiteralPath $dlcRoot -PathType Container)){Write-Warning "DLC folder not found: $dlcRoot"}
    else{
        $dlcContent=Join-Path $UserDataRoot '0000000000000000/4D530910/00000002'
        $dlcPackages=@(Get-ChildItem -LiteralPath $dlcRoot -Directory)
        $dlcNested=Join-Path $dlcRoot '00000002'
        if(Test-Path -LiteralPath $dlcNested -PathType Container){$dlcPackages+=@(Get-ChildItem -LiteralPath $dlcNested -Directory)}
        $dlcLinked=0;$dlcPresent=0
        foreach($package in $dlcPackages){
            if($package.Name -notmatch '^([0-9A-Fa-f]{16}|[0-9A-Fa-f]{42})$' -or -not(Test-Path -LiteralPath (Join-Path $package.FullName 'Media') -PathType Container)){continue}
            $target=Join-Path $dlcContent $package.Name
            if(Test-Path -LiteralPath $target){$dlcPresent++;continue}
            if(-not $ValidateOnly){
                New-Item -ItemType Directory -Path $dlcContent -Force|Out-Null
                New-Item -ItemType Junction -Path $target -Target $package.FullName|Out-Null
            }
            $dlcLinked++
        }
        Write-Host "DLC: $dlcLinked added, $dlcPresent already installed."
    }
}
# Content headers (added 2026-10-23): FM4 only mounts marketplace packages that the runtime
# lists from a header file; for a package folder without one the game is handed stale data and
# skips it. Write a standard header (HDD device, marketplace type, FM4 title, no license) for
# every installed package that lacks one. Genuine headers are never touched.
$contentRoot=Join-Path $UserDataRoot '0000000000000000/4D530910'
$packageRoot=Join-Path $contentRoot '00000002'
if(Test-Path -LiteralPath $packageRoot -PathType Container){
    $headerRoot=Join-Path $contentRoot 'Headers/00000002'
    $headersWritten=0
    foreach($package in Get-ChildItem -LiteralPath $packageRoot -Directory){
        if($package.Name -notmatch '^([0-9A-Fa-f]{16}|[0-9A-Fa-f]{42})$'){continue}
        $headerPath=Join-Path $headerRoot ($package.Name+'.header')
        if(Test-Path -LiteralPath $headerPath){continue}
        if(-not $ValidateOnly){
            $bytes=New-Object byte[] 328
            $bytes[3]=1;$bytes[7]=2                      # device 1 (HDD), content type 2 (marketplace), big-endian
            $display=[Text.Encoding]::BigEndianUnicode.GetBytes('FM4 content '+$package.Name.Substring(0,8))
            [Array]::Copy($display,0,$bytes,8,[Math]::Min($display.Length,256))
            $name=[Text.Encoding]::ASCII.GetBytes($package.Name)
            [Array]::Copy($name,0,$bytes,0x108,[Math]::Min($name.Length,42))
            $bytes[0x140]=0x4D;$bytes[0x141]=0x53;$bytes[0x142]=0x09;$bytes[0x143]=0x10  # title 4D530910
            New-Item -ItemType Directory -Path $headerRoot -Force|Out-Null
            [IO.File]::WriteAllBytes($headerPath,$bytes)
        }
        $headersWritten++
    }
    if($headersWritten){Write-Host "DLC setup complete: $headersWritten packs."}
}
$gameArguments = @("--game_data_root=$game")
$gameArguments += "--user_data_root=$UserDataRoot"
$disc2=if($Disc2Root){$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Disc2Root)}else{Join-Path $root '.local/disc2-content'}
$start.EnvironmentVariables['FM4_DISC2_ROOT']=$disc2
$gameArguments += "--fm4_disc2_content_root=$disc2"
$start.EnvironmentVariables.Remove('FM4_TRACE_INPUT')|Out-Null
$start.EnvironmentVariables.Remove('FM4_FRAME_METRICS')|Out-Null
$start.EnvironmentVariables.Remove('FM4_TEXTURE_DUMP')|Out-Null
$start.EnvironmentVariables.Remove('FM4_TEXTURE_PACK')|Out-Null
$start.EnvironmentVariables.Remove('FM4_TEXTURE_RGBA8')|Out-Null
$start.EnvironmentVariables.Remove('FM4_BACKGROUND')|Out-Null
$start.EnvironmentVariables.Remove('FM4_CAPTURE_DIR')|Out-Null
$start.EnvironmentVariables.Remove('FM4_DIAGNOSTIC_INPUT')|Out-Null
$start.EnvironmentVariables.Remove('FM4_WHEEL_DIAGNOSTIC_INPUT')|Out-Null
$start.EnvironmentVariables.Remove('FM4_WHEEL_DIAGNOSTIC_DEVICES')|Out-Null
$start.EnvironmentVariables.Remove('FM4_QUIET_CAPTURE')|Out-Null
$start.EnvironmentVariables.Remove('FM4_CAPTURE_ROV_DEPTH')|Out-Null
$start.EnvironmentVariables.Remove('FM4_SCENE_DEPTH_SHADER')|Out-Null
$start.EnvironmentVariables.Remove('FM4_DIAGNOSTIC_DRED')|Out-Null
if($DiagnosticDred){$start.EnvironmentVariables['FM4_DIAGNOSTIC_DRED']='1'}
$start.EnvironmentVariables.Remove('FM4_TEMPORAL_MODE')|Out-Null
$start.EnvironmentVariables.Remove('FM4_TEMPORAL_WIDTH')|Out-Null
$start.EnvironmentVariables.Remove('FM4_TEMPORAL_HEIGHT')|Out-Null
$start.EnvironmentVariables.Remove('FM4_TEMPORAL_SHADER')|Out-Null
$start.EnvironmentVariables.Remove('FM4_STREAMLINE_DIR')|Out-Null
$start.EnvironmentVariables.Remove('FM4_FSR_DIR')|Out-Null
foreach($key in @('FM4_NEURAL_ENABLE','FM4_NEURAL_DIR','FM4_NEURAL_BRIDGE_DIR','FM4_NEURAL_PATH','FM4_NEURAL_PASSES','FM4_NEURAL_PRESET','FM4_NEURAL_STYLE','FM4_NEURAL_STRENGTH')){$start.EnvironmentVariables.Remove($key)|Out-Null}
if($temporalPipeline){
    $start.EnvironmentVariables['FM4_TEMPORAL_MODE']=if($temporal){$Upscaler}else{'SPATIAL'}
    $start.EnvironmentVariables['FM4_TEMPORAL_WIDTH']=[string](1280*$outputScale)
    $start.EnvironmentVariables['FM4_TEMPORAL_HEIGHT']=[string](720*$outputScale)
    $start.EnvironmentVariables['FM4_TEMPORAL_SHADER']=Join-Path $root 'tools/graphics/temporal.cs.hlsl'
    $start.EnvironmentVariables['FM4_SCENE_DEPTH_SHADER']=Join-Path $root 'tools/graphics/scene_depth.cs.hlsl'
    if($Upscaler -eq 'DLSS'){
        $plugins=Join-Path $root '.local/streamline-current/bin/x64'
        if(-not(Test-Path -LiteralPath (Join-Path $plugins 'sl.interposer.dll'))){throw 'Install the current signed Streamline runtime in .local/streamline-current/bin/x64.'}
        $start.EnvironmentVariables['FM4_STREAMLINE_DIR']=$plugins
    } elseif($Upscaler -eq 'FSR') {
        $plugins=Join-Path $root '.local/fsr-sdk/Kits/FidelityFX/signedbin'
        if(-not(Test-Path -LiteralPath (Join-Path $plugins 'amd_fidelityfx_loader_dx12.dll'))){throw 'Install the current signed FidelityFX SDK in .local/fsr-sdk.'}
        $start.EnvironmentVariables['FM4_FSR_DIR']=$plugins
        Copy-Item -LiteralPath (Join-Path $plugins 'amd_fidelityfx_upscaler_dx12.dll') -Destination $build -Force
    }
    if($NeuralRendering){
        $neuralFolder=$NeuralRuntimeFolder
        $neuralBridge=Join-Path $root '.local/graphics-build'
        $neuralBridgeBuilt=Test-Path -LiteralPath (Join-Path $neuralBridge 'nvngx.dll_fm4.dll')
        if(-not $neuralPlan.Supported){throw ($neuralPlan.Warnings -join ' ')}
        if(-not(Test-Path -LiteralPath (Join-Path $neuralFolder 'nvngx_dlssnr.dll'))){throw "Add a compatible nvngx_dlssnr.dll to: $neuralFolder"}
        if(-not(Test-Path -LiteralPath (Join-Path $neuralFolder 'nvngx_dlss.dll'))){throw "Add the matching nvngx_dlss.dll to: $neuralFolder"}
        # The community path always needs the source-built bridge; the official path
        # uses it only as a fallback when the driver declines feature 18.
        if($neuralPlan.Path -eq 'community' -and -not $neuralBridgeBuilt){throw 'Neural Rendering support is missing; run tools/Build-Graphics.cmd.'}
        $start.EnvironmentVariables['FM4_NEURAL_ENABLE']='1'
        $start.EnvironmentVariables['FM4_NEURAL_DIR']=$neuralFolder
        $start.EnvironmentVariables['FM4_NEURAL_BRIDGE_DIR']=$neuralBridge
        $start.EnvironmentVariables['FM4_NEURAL_PATH']=$neuralPlan.Path
        $start.EnvironmentVariables['FM4_NEURAL_PASSES']=[string]$NeuralPasses
        $start.EnvironmentVariables['FM4_NEURAL_PRESET']=[string]$NeuralPreset
        $start.EnvironmentVariables['FM4_NEURAL_STYLE']=[string]$NeuralStyle
        $start.EnvironmentVariables['FM4_NEURAL_STRENGTH']=$NeuralStrength.ToString([Globalization.CultureInfo]::InvariantCulture)
    }
}
if($CaptureRovDepth){
    $start.EnvironmentVariables['FM4_CAPTURE_ROV_DEPTH']='1'
    $start.EnvironmentVariables['FM4_SCENE_DEPTH_SHADER']=Join-Path $root 'tools/graphics/scene_depth.cs.hlsl'
}
$start.EnvironmentVariables.Remove('FM4_SINGLE_RESOLVE_BUFFER')|Out-Null
if($Background){$start.EnvironmentVariables['FM4_BACKGROUND']='1'}
if($DiagnosticInput){
    # Exercise the same source selection/capability/layout path as normal play,
    # rather than bypassing the game's input backend in the renderer SDK.
    $start.EnvironmentVariables['FM4_WHEEL_DIAGNOSTIC_INPUT']=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DiagnosticInput)
    $start.EnvironmentVariables['FM4_WHEEL_DIAGNOSTIC_DEVICES']=[string]$DiagnosticDevices
}
if($Background -and ($TraceGraphics -or $CaptureOutput)){
    $captureFolder=Join-Path $runFolder $runId
    New-Item -ItemType Directory -Path $captureFolder -Force|Out-Null
    $start.EnvironmentVariables['FM4_CAPTURE_DIR']=$captureFolder
}
if($Metrics){$start.EnvironmentVariables['FM4_FRAME_METRICS']='1';$start.EnvironmentVariables['FM4_SPEED_TRACE']='1'}
if($TraceInput){$start.EnvironmentVariables['FM4_TRACE_INPUT']='1'}
if($DumpTextures){
    $dumpFolder=if($TextureDumpFolder){$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($TextureDumpFolder)}else{Join-Path $root '.local/texture-dumps'}
    $start.EnvironmentVariables['FM4_TEXTURE_DUMP']=$dumpFolder
}
$pack=Join-Path $root '.local/texture-pack/replacements'
if(Test-Path -LiteralPath (Join-Path $root 'textures/Enhanced/replacements')){$pack=Join-Path $root 'textures/Enhanced/replacements'}
if($TexturePack){
    $pack=(Resolve-Path -LiteralPath $TexturePack -ErrorAction Stop).Path
    if(-not(Test-Path -LiteralPath $pack -PathType Container)){throw 'Choose an extracted texture pack folder.'}
    if(Test-Path -LiteralPath (Join-Path $pack 'replacements') -PathType Container){$pack=Join-Path $pack 'replacements'}
}
if(-not $NoTextures -and (Test-Path -LiteralPath $pack)){$start.EnvironmentVariables['FM4_TEXTURE_PACK']=$pack}
if($EnableRgbaTextures -and -not $NoTextures){$start.EnvironmentVariables['FM4_TEXTURE_RGBA8']='1'}
Write-Host "Render resolution: $([int]$scale*1280)x$([int]$scale*720); GPU: $(if($AdapterName){$AdapterName}else{'automatic'})."
Write-Host "Rendering mode: $Upscaler."
if($temporal){Write-Host "${Upscaler}: 1280x720 to $(1280*$outputScale)x$(720*$outputScale)."}
if($scale -eq 2){Write-Host 'Rendering at 2560x1440.'}
if($scale -ge 3){Write-Host 'Rendering at 3840x2160.'}
if(-not $ExperimentalReadbackBatch){Write-Warning 'Graphics copy batching is off; races may run slower.'}
Write-Host "User data: $UserDataRoot"
$driverInfo=@()
$nvidiaSmi=Get-Command nvidia-smi -ErrorAction SilentlyContinue
if($nvidiaSmi){
    $driverStart=[Diagnostics.ProcessStartInfo]::new($nvidiaSmi.Source)
    $driverStart.UseShellExecute=$false
    $driverStart.CreateNoWindow=$true
    $driverStart.RedirectStandardOutput=$true
    $driverStart.RedirectStandardError=$true
    Set-Fm4ProcessArguments -StartInfo $driverStart -ArgumentList @('--query-gpu=name,driver_version','--format=csv,noheader')
    $driverProcess=[Diagnostics.Process]::Start($driverStart)
    try{
        $driverOutput=$driverProcess.StandardOutput.ReadToEndAsync()
        $driverErrors=$driverProcess.StandardError.ReadToEndAsync()
        if($driverProcess.WaitForExit(5000)){
            if($driverProcess.ExitCode -eq 0){$driverInfo=@($driverOutput.GetAwaiter().GetResult() -split '\r?\n' | Where-Object {$_})}
        }else{$driverProcess.Kill();$driverProcess.WaitForExit()}
    }finally{$driverProcess.Dispose()}
}
Set-Fm4ProcessArguments -StartInfo $start -ArgumentList $gameArguments
$process=$null
try{
if($forzaPlus){
    # The backend holds the edition admission lock until this child owns the game lease.
    $childEnvironment=@{};$fm4Keys=@()
    foreach($key in @($start.EnvironmentVariables.Keys)){if($key -like 'FM4_*'){$fm4Keys+=$key;$childEnvironment[$key]=$start.EnvironmentVariables[$key]}}
    $remove=@(Get-ChildItem Env: | Where-Object { $_.Name -like 'FM4_*' -and $_.Name -notin $fm4Keys } | ForEach-Object Name)
    if(-not $start.EnvironmentVariables.ContainsKey('SDL_AUDIODRIVER')){$remove+='SDL_AUDIODRIVER'}
    $launched=Invoke-Pfp4Backend -Operation launch -EditionRoot $root -Settings $forzaPlus -BackendCliPath $pfpBackend `
        -GameExecutable $start.FileName -GameArguments $gameArguments -GameEnvironment $childEnvironment -GameEnvironmentRemove $remove
    $process=[Diagnostics.Process]::GetProcessById([int]$launched.pid)
    $null=$process.Handle  # keep a handle so ExitCode stays readable after exit
} else {
$process=[Diagnostics.Process]::Start($start)
}
$receipt=@{profile=$Profile;upscaler=$Upscaler;batch=[bool]$ExperimentalReadbackBatch;hostTargets=[bool]$HostRenderTargets;readbackHeap=[bool]$ReadbackHeap;started=(Get-Date).ToString('o');pid=$process.Id;log=$runLog;runtimeSha256=(Get-FileHash (Join-Path $build 'rexruntime.dll')).Hash;gpuSha256=(Get-FileHash (Join-Path $build 'rexgpu-xenos.dll')).Hash}
$receipt.background=[bool]$Background
$receipt.headlessDialogs=[bool]$Background
$receipt.captureFolder=if($Background -and ($TraceGraphics -or $CaptureOutput)){$captureFolder}else{$null}
$receipt.diagnosticInput=$DiagnosticInput
$receipt.diagnosticDevices=if($DiagnosticInput){$DiagnosticDevices}else{$null}
$receipt.userDataRoot=$UserDataRoot
$receipt.forzaPlus=if($forzaPlus){@{mode=$forzaPlus.mode;receiptId=$forzaPlusState.receipt_id;generation=$forzaPlusState.generation;editionRoot=$root}}else{$null}
$receipt.nvidiaDriverInfo=$driverInfo
$receipt.gameBuildRoot=$build
$receipt.sdkInstallRoot=$SdkInstallRoot
$receipt.depthDiagnostics=[bool]$CaptureRovDepth
$receipt.dred=[bool]$DiagnosticDred
$receipt.executableSha256=(Get-FileHash $start.FileName).Hash
$receipt.internalWidth=1280*[int]$scale
$receipt.internalHeight=720*[int]$scale
$receipt.temporal=[bool]$temporal
$receipt.neuralRendering=[bool]$NeuralRendering
$receipt.neuralPasses=$NeuralPasses
$receipt.neuralPreset=$NeuralPreset
$receipt.neuralStyle=$NeuralStyle
$receipt.neuralStrength=$NeuralStrength
if($temporalPipeline){
    $receipt.temporalShaderSha256=(Get-FileHash (Join-Path $root 'tools/graphics/temporal.cs.hlsl')).Hash
    $receipt.sceneDepthShaderSha256=(Get-FileHash (Join-Path $root 'tools/graphics/scene_depth.cs.hlsl')).Hash
}
if($NeuralRendering){
    $receipt.neuralRuntimeSha256=(Get-FileHash (Join-Path $neuralFolder 'nvngx_dlssnr.dll')).Hash
    $receipt.neuralInternalDlssSha256=(Get-FileHash (Join-Path $neuralFolder 'nvngx_dlss.dll')).Hash
    $receipt.neuralBridgeSha256=if($neuralBridgeBuilt){(Get-FileHash (Join-Path $neuralBridge 'nvngx.dll_fm4.dll')).Hash}else{$null}
    $receipt.neuralPathRequested=$neuralPlan.Requested
    $receipt.neuralPath=$neuralPlan.Path
    $receipt.neuralPathTested=$neuralPlan.Tested
    $receipt.neuralGpu=$neuralPlan.Adapter
    $receipt.neuralGpuGeneration=$neuralPlan.Generation
}
$receipt.runtimeConfigSha256=(Get-FileHash (Join-Path $build 'fm4.toml')).Hash
$receipt.outputWidth=if($temporalPipeline){1280*$outputScale}else{1280*[int]$scale}
$receipt.outputHeight=if($temporalPipeline){720*$outputScale}else{720*[int]$scale}
$receipt.adapter=$AdapterName
$receipt.settingsFile=[IO.Path]::GetFullPath($SettingsFile)
$receipt.texturePack=if($NoTextures){$null}else{$pack}
$receipt.textureMode=if($PSBoundParameters.ContainsKey('TexturePack') -or $PSBoundParameters.ContainsKey('NoTextures')){'explicit'}else{$textureMode}
$receipt.texturePackFinished=[bool]$texturePackFinished
$receipt.textureDumpFolder=if($DumpTextures){$dumpFolder}else{$null}
$receipt.rgbaTextures=[bool]$EnableRgbaTextures
$receipt.readbackCenterSample=[bool]$ReadbackCenterSample
$receipt.runSeconds=$RunSeconds
$receipt | ConvertTo-Json | Write-Fm4Utf8File -Path (Join-Path $runFolder "$runId.json")
    $boundedStop=$false
    $requestedStop=$false
    if($Background){
        $stopRequest=Join-Path $runFolder "$runId.stop"
        $deadline=if($RunSeconds -gt 0){[DateTime]::UtcNow.AddSeconds($RunSeconds)}else{[DateTime]::MaxValue}
        while(-not $process.WaitForExit(500)){
            $requestedStop=Test-Path -LiteralPath $stopRequest
            $boundedStop=[DateTime]::UtcNow -ge $deadline
            if($requestedStop -or $boundedStop){
                $process.Kill(); $process.WaitForExit(); break
            }
        }
    } elseif($RunSeconds -gt 0){
        if(-not $process.WaitForExit($RunSeconds*1000)){
            # Stop only the exact child created above. Hidden diagnostics never
            # need to activate the desktop or use keyboard/window automation.
            $process.Kill()
            $process.WaitForExit()
            $boundedStop=$true
        }
    } else {$process.WaitForExit()}
    $receipt.boundedStop=$boundedStop
    $receipt.requestedStop=$requestedStop
    $receipt.exitCode=$process.ExitCode
    $receipt.cleanClose=Test-Fm4CleanClose $process.ExitCode $runLog
    $receipt.finished=(Get-Date).ToString('o')
    $receipt | ConvertTo-Json | Write-Fm4Utf8File -Path (Join-Path $runFolder "$runId.json")
    if($process.ExitCode -ne 0 -and -not $receipt.cleanClose -and -not $boundedStop -and -not $requestedStop){throw "FM4 exited with $($process.ExitCode); see $runLog"}
} finally {
    if($null -ne $process){
        try{if(-not $process.HasExited){$process.Kill();$process.WaitForExit()}}
        finally{$process.Dispose()}
    }
}
