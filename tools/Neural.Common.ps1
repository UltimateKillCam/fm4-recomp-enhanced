# DLSS 5 Neural Rendering path selection. Modified 2026-10-20. GPL-3.0-only.
# Shared by the Enhanced settings window and Run-Enhanced.ps1 (Windows PowerShell 5.1
# and PowerShell 7). Read-only: it looks at the display adapters and never changes
# drivers, system files or settings. No NVIDIA runtime is included or downloaded.
#
# Two runtime paths exist:
# - official  (RTX 50 / Blackwell): NVIDIA's own nvngx_dlssnr.dll is loaded by the
#   driver's NGX core as feature 18 (NVSDK_NGX_D3D12_CreateFeature). If the driver
#   declines the feature for this application, the host falls back to the
#   source-built direct bridge when it has been built. Not yet tested on RTX 50
#   hardware in this project.
# - community (RTX 40 / Ada): an Ada-compatible community runtime is driven through
#   the source-built direct bridge (nvngx.dll_fm4.dll). Tested on an RTX 4080.

$script:Fm4NeuralOfficialDriver = [version]'616.64'

# Classifies a display adapter name. GeForce model numbers are matched exactly so
# workstation names such as "Quadro RTX 4000" (Turing) or "RTX 5000 Ada" (Ada) are not
# mistaken for GeForce RTX 40/50 cards.
function Get-Fm4GpuGeneration([string]$Name) {
    $n = if ($Name) { $Name.ToUpperInvariant() } else { '' }
    if (-not $n) { return @{ Key = 'unknown'; Label = 'not detected' } }
    if ($n -notmatch 'NVIDIA|GEFORCE|QUADRO|\bRTX\b|\bGTX\b|TESLA') { return @{ Key = 'non-nvidia'; Label = 'not an NVIDIA GPU' } }
    if ($n -match '\bBLACKWELL\b' -or $n -match '\bRTX\s*50[5-9]0\b') { return @{ Key = 'blackwell'; Label = 'RTX 50 (Blackwell)' } }
    if ($n -match '\bADA\b' -or $n -match '\bRTX\s*40[5-9]0\b') { return @{ Key = 'ada'; Label = 'RTX 40 (Ada)' } }
    if ($n -match '\bRTX\s*30[5-9]0\b' -or $n -match '\bRTX\s*A\d{4}\b') { return @{ Key = 'ampere'; Label = 'RTX 30 (Ampere)' } }
    if ($n -match '\bRTX\s*20[6-8]0\b' -or $n -match 'QUADRO RTX') { return @{ Key = 'turing'; Label = 'RTX 20 (Turing)' } }
    return @{ Key = 'other-nvidia'; Label = 'NVIDIA GPU without DLSS 5 support' }
}

# Windows reports NVIDIA drivers as e.g. 32.0.16.1742; the NVIDIA version is 617.42.
function ConvertTo-Fm4NvidiaDriverVersion([string]$WindowsVersion) {
    $parts = @($WindowsVersion -split '\.')
    if ($parts.Count -lt 4) { return $null }
    $digits = ($parts[2] + $parts[3]) -replace '\D', ''
    if ($digits.Length -lt 5) { return $null }
    $digits = $digits.Substring($digits.Length - 5)
    return [version]('{0}.{1}' -f [int]$digits.Substring(0, 3), $digits.Substring(3, 2))
}

function Get-Fm4DisplayAdapters {
    try {
        return @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop | ForEach-Object {
            @{ Name = [string]$_.Name; DriverVersion = [string]$_.DriverVersion }
        })
    } catch { return @() }
}

# Decides which runtime path Neural Rendering will use. Requested is the saved
# neural_path value: auto, official or community. AdapterName matches the GPU
# selector (part of the adapter name; empty picks the first NVIDIA adapter).
# Adapters can be supplied for tests; otherwise the installed adapters are read.
function Resolve-Fm4NeuralPath {
    param([string]$Requested = 'auto', [string]$AdapterName = '', [object[]]$Adapters = $null)
    if ($Requested -notin @('auto', 'official', 'community')) { throw "neural_path must be auto, official or community (got '$Requested')." }
    if ($null -eq $Adapters) { $Adapters = Get-Fm4DisplayAdapters }
    $adapter = $null
    if ($AdapterName) { $adapter = @($Adapters | Where-Object { $_.Name -and $_.Name.IndexOf($AdapterName, [StringComparison]::OrdinalIgnoreCase) -ge 0 })[0] }
    if (-not $adapter) { $adapter = @($Adapters | Where-Object { (Get-Fm4GpuGeneration $_.Name).Key -notin @('non-nvidia', 'unknown') })[0] }
    if (-not $adapter) { $adapter = @($Adapters)[0] }
    $name = if ($adapter) { [string]$adapter.Name } else { '' }
    $generation = Get-Fm4GpuGeneration $name
    $driver = if ($adapter -and $generation.Key -ne 'non-nvidia') { ConvertTo-Fm4NvidiaDriverVersion ([string]$adapter.DriverVersion) } else { $null }
    $warnings = @()
    $supported = $true
    $path = $Requested
    if ($Requested -eq 'auto') {
        switch ($generation.Key) {
            'blackwell' { $path = 'official' }
            'ada' { $path = 'community' }
            { $_ -in @('ampere', 'turing') } {
                $path = 'community'
                $warnings += "$($generation.Label) is untested here; it needs a community runtime built for this GPU and costs much more GPU time."
            }
            'unknown' { $path = 'community'; $warnings += 'The GPU could not be detected; assuming the community path.' }
            default { $path = ''; $supported = $false }
        }
    } elseif ($generation.Key -in @('non-nvidia', 'other-nvidia')) {
        $supported = $false
    }
    if (-not $supported) {
        $path = ''
        $warnings += "Neural Rendering needs an NVIDIA RTX GPU; detected: $(if ($name) { $name } else { 'no GPU' })."
    }
    if ($path -eq 'official') {
        if ($generation.Key -ne 'blackwell') {
            $warnings += 'The official path is meant for RTX 50. On other GPUs the driver is expected to decline; the direct bridge is then used if it is built.'
        }
        if ($driver -and $driver -lt $script:Fm4NeuralOfficialDriver) {
            $warnings += "NVIDIA lists driver $($script:Fm4NeuralOfficialDriver) or newer for DLSS 5; this GPU reports $driver."
        }
    }
    $tested = ($path -eq 'community' -and $generation.Key -eq 'ada')
    $pathLabel = switch ($path) {
        'official' { 'Official NVIDIA path' }
        'community' { 'Community compatibility path' }
        default { 'Not available' }
    }
    $detail = switch ($path) {
        'official' { "NVIDIA's driver runs NVIDIA's own runtime; if it declines, the direct bridge is used when built." }
        'community' { 'A community Ada-compatible runtime runs through the source-built direct bridge.' }
        default { '' }
    }
    $testing = if ($path -eq 'official') { 'untested on RTX 50 hardware' }
               elseif ($tested) { 'tested on an RTX 4080' }
               elseif ($path) { 'untested on this GPU' } else { '' }
    $gpuText = if ($name) { "$name · $($generation.Label)" + $(if ($driver) { " · driver $driver" } else { '' }) } else { 'GPU not detected' }
    $summary = "$gpuText  →  $pathLabel" + $(if ($testing) { " ($testing)" } else { '' })
    return [pscustomobject]@{
        Requested = $Requested; Path = $path; PathLabel = $pathLabel; Supported = $supported; Tested = $tested
        Adapter = $name; Generation = $generation.Key; GenerationLabel = $generation.Label; Driver = $driver
        Summary = $summary; Detail = $detail; Warnings = $warnings
    }
}
