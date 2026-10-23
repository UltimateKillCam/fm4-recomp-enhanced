# Wheel/controller integration. Modified 2026-10-18. GPL-3.0-only.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GameDataRoot,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$')][string]$WorkspaceName,
    [string]$Disc2ContentRoot,
    [string]$UserDataRoot,
    [switch]$Background,
    [string]$DiagnosticInput,
    [ValidateRange(0,3)][int]$DiagnosticDevices=3,
    [switch]$TraceInput,
    [ValidateRange(0,3600)][int]$RunSeconds=0
)

. (Join-Path $PSScriptRoot "Release.Common.ps1")

$root = Get-Fm4RepositoryRoot
if($DiagnosticInput -and -not $Background){throw 'Diagnostic input requires a hidden background run.'}
if($PSBoundParameters.ContainsKey('DiagnosticDevices') -and -not $DiagnosticInput){throw 'DiagnosticDevices requires DiagnosticInput.'}
$resolvedGameRoot = Assert-Fm4GameInputs -GameDataRoot $GameDataRoot
$localRoot = Join-Path $root '.local'
if ($WorkspaceName) { $localRoot = Join-Path $localRoot $WorkspaceName }
$buildRoot = Join-Path $localRoot "work/fm4/out/build/win-amd64-release"
$executable = Join-Path $buildRoot "fm4.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Local build not found. Run tools/Build-Local.ps1 first."
}
$runId=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$runs=Join-Path $localRoot 'runs'
[IO.Directory]::CreateDirectory($runs)|Out-Null
$runLog=Join-Path $runs "$runId.log"

$runtimeText=Get-Content -LiteralPath (Join-Path $localRoot 'work/fm4/fm4_runtime.toml') -Raw -Encoding UTF8
# The runtime appends to its configured log. Give each child a unique path so
# gameplay evidence never includes an earlier launch's entries.
$logLine='log_file = "'+$runLog.Replace('\','/')+'"'
if([regex]::IsMatch($runtimeText,'(?m)^log_file\s*=.*$')){
    $runtimeText=[regex]::Replace($runtimeText,'(?m)^log_file\s*=.*$',$logLine)
}else{$runtimeText+="`n$logLine`n"}
if($Background){
    foreach($entry in @(@('fullscreen','false'),@('audio_mute','true'),@('mnk_mode','false'),@('headless','true'))){
        $pattern='(?m)^'+$entry[0]+'\s*=.*$'
        $line=$entry[0]+' = '+$entry[1]
        if([regex]::IsMatch($runtimeText,$pattern)){$runtimeText=[regex]::Replace($runtimeText,$pattern,$line)}
        else{$runtimeText+="`n$line`n"}
    }
}
Write-Fm4Utf8File -Path (Join-Path $buildRoot 'fm4.toml') -Text $runtimeText
$localUserRoot = if($UserDataRoot){[IO.Path]::GetFullPath($UserDataRoot)}else{Join-Path $localRoot 'user'}
[IO.Directory]::CreateDirectory($localUserRoot) | Out-Null
$arguments = @(
    "--game_data_root=$resolvedGameRoot",
    "--user_data_root=$localUserRoot"
)
if ($Disc2ContentRoot) {
    $resolvedDisc2Root = [IO.Path]::GetFullPath($Disc2ContentRoot)
    if (-not (Test-Path -LiteralPath $resolvedDisc2Root -PathType Container)) {
        throw "Disc 2 content folder not found: $resolvedDisc2Root"
    }
    $arguments += "--fm4_disc2_content_root=$resolvedDisc2Root"
}

# PowerShell does not wait for a Windows GUI executable invoked with '&'.
# ProcessStartInfo preserves argument boundaries and gives the launcher a real
# exit code after the game closes.
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $executable
$startInfo.WorkingDirectory = $buildRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
foreach($key in @('FM4_BACKGROUND','FM4_CAPTURE_DIR','FM4_WHEEL_DIAGNOSTIC_INPUT','FM4_WHEEL_DIAGNOSTIC_DEVICES','FM4_QUIET_CAPTURE','FM4_TRACE_INPUT')){
    $startInfo.EnvironmentVariables.Remove($key)|Out-Null
}
$captureFolder=Join-Path $runs $runId
if($Background){
    [IO.Directory]::CreateDirectory($captureFolder)|Out-Null
    $startInfo.EnvironmentVariables['FM4_BACKGROUND']='1'
    $startInfo.EnvironmentVariables['FM4_QUIET_CAPTURE']='1'
    $startInfo.EnvironmentVariables['FM4_CAPTURE_DIR']=$captureFolder
    # audio_mute above zeroes samples in the pinned SDL audio callback. Its
    # Windows build does not include SDL's optional dummy audio backend.
    $startInfo.EnvironmentVariables.Remove('SDL_AUDIODRIVER')|Out-Null
    if($DiagnosticInput){
        $startInfo.EnvironmentVariables['FM4_WHEEL_DIAGNOSTIC_INPUT']=[IO.Path]::GetFullPath($DiagnosticInput)
        $startInfo.EnvironmentVariables['FM4_WHEEL_DIAGNOSTIC_DEVICES']=[string]$DiagnosticDevices
    }
}
if($TraceInput){$startInfo.EnvironmentVariables['FM4_TRACE_INPUT']='1'}
Set-Fm4ProcessArguments -StartInfo $startInfo -ArgumentList $arguments
if ($Disc2ContentRoot) {
    # The runtime loads fm4.toml after initial path resolution. The environment
    # override also reaches Disc 2 discovery when that file contains an empty cvar.
    $startInfo.EnvironmentVariables['FM4_DISC2_ROOT'] = $resolvedDisc2Root
}
$runtimeHash=(Get-FileHash -LiteralPath (Join-Path $buildRoot 'rexruntime.dll')).Hash
$gpuHash=(Get-FileHash -LiteralPath (Join-Path $buildRoot 'rexgpu-xenos.dll')).Hash
$configHash=(Get-FileHash -LiteralPath (Join-Path $buildRoot 'fm4.toml')).Hash
$executableHash=(Get-FileHash -LiteralPath $executable).Hash
$gameProcess=$null
try {
$gameProcess = [Diagnostics.Process]::Start($startInfo)
$receipt=[ordered]@{
    started=(Get-Date).ToString('o');pid=$gameProcess.Id;background=[bool]$Background
    headlessDialogs=[bool]$Background
    gameBuildRoot=$buildRoot;captureFolder=$captureFolder;diagnosticInput=$DiagnosticInput
    diagnosticDevices=$DiagnosticDevices
    userDataRoot=$localUserRoot;runSeconds=$RunSeconds
    executableSha256=$executableHash;log=$runLog
    runtimeSha256=$runtimeHash;gpuSha256=$gpuHash;runtimeConfigSha256=$configHash
    processPriority=[string]$gameProcess.PriorityClass
}
$receiptFile=Join-Path $runs "$runId.json"
$receipt|ConvertTo-Json|Write-Fm4Utf8File -Path $receiptFile
    $expectedStop=$false
    $deadline=if($RunSeconds){[DateTime]::UtcNow.AddSeconds($RunSeconds)}else{[DateTime]::MaxValue}
    while(-not $gameProcess.WaitForExit(500)){
        if([DateTime]::UtcNow -ge $deadline -or ($Background -and (Test-Path -LiteralPath (Join-Path $runs "$runId.stop")))){
            $gameProcess.Kill();$gameProcess.WaitForExit();$expectedStop=$true;break
        }
    }
    if(Test-Path -LiteralPath $runLog -PathType Leaf){
        $receipt.logSha256=(Get-FileHash -LiteralPath $runLog).Hash
    }
    $receipt.finished=(Get-Date).ToString('o');$receipt.exitCode=$gameProcess.ExitCode;$receipt.expectedStop=$expectedStop
    $receipt|ConvertTo-Json|Write-Fm4Utf8File -Path $receiptFile
    if ($gameProcess.ExitCode -ne 0 -and -not $expectedStop) { throw "FM4 exited with code $($gameProcess.ExitCode). See $runLog." }
} finally {
    if($null -ne $gameProcess){
        try{if(-not $gameProcess.HasExited){$gameProcess.Kill();$gameProcess.WaitForExit()}}
        finally{$gameProcess.Dispose()}
    }
}
