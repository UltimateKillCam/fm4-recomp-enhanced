[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceHeaders,
    [Parameter(Mandatory)][string]$UserDataRoot,
    [switch]$Apply
)
$ErrorActionPreference='Stop'
# Runtime export format: 0x148 bytes of aggregate metadata followed by a native
# uint32 license mask. An extraction without that tail has no recoverable grant.
$source=(Get-Item -LiteralPath $SourceHeaders -ErrorAction Stop).FullName
$userRoot=[IO.Path]::GetFullPath($UserDataRoot)
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$privateRoot=Join-Path $projectRoot '.local'
if(-not $userRoot.StartsWith($privateRoot+'\',[StringComparison]::OrdinalIgnoreCase)){
    throw 'Header destination must be a private profile inside this project .local directory.'
}
if($Apply){
    foreach($game in Get-CimInstance Win32_Process -Filter "Name='fm4.exe'"){
        $command=[string]$game.CommandLine
        if(-not $command -or $command -notmatch '--user_data_root=' -or
           $command.Replace('/','\').IndexOf($userRoot,[StringComparison]::OrdinalIgnoreCase) -ge 0){
            throw 'Close the game using this profile before importing content metadata.'
        }
    }
}
$contentRoot=Join-Path $userRoot '0000000000000000/4D530910'
$headers=Join-Path $contentRoot 'Headers/00000002'
$plans=@()
foreach($file in Get-ChildItem -LiteralPath $source -File -Filter '*.header'){
    $name=$file.BaseName
    if($name -notmatch '^([0-9a-f]{16}|[0-9a-f]{42})$'){throw "Invalid package identity: $($file.Name)"}
    $bytes=[IO.File]::ReadAllBytes($file.FullName)
    if($bytes.Length -ne 332){throw "No explicit package license mask in $($file.Name); expected a 332-byte runtime export."}
    $stored=[Text.Encoding]::ASCII.GetString($bytes,0x108,42).TrimEnd([char]0)
    $title=($bytes[0x140..0x143]|ForEach-Object {$_.ToString('X2')}) -join ''
    $type=($bytes[4..7]|ForEach-Object {$_.ToString('X2')}) -join ''
    $device=($bytes[0..3]|ForEach-Object {$_.ToString('X2')}) -join ''
    if($stored -ine $name -or $title -ne '4D530910' -or $type -ne '00000002' -or $device -ne '00000001'){
        throw "Header title/type/device/identity mismatch: $($file.Name)"
    }
    if(@($bytes[0x134..0x13B]|Where-Object {$_ -ne 0}).Count){throw "Marketplace header is not common content: $($file.Name)"}
    $package=Join-Path (Join-Path $contentRoot '00000002') $name
    if(-not(Test-Path -LiteralPath (Join-Path $package 'Media') -PathType Container)){
        throw "The corresponding extracted package is not installed: $name"
    }
    $target=Join-Path $headers $file.Name
    if(Test-Path -LiteralPath $target){
        if((Get-Item -LiteralPath $target).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Header destination is a link: $target"}
        if((Get-FileHash -LiteralPath $target).Hash -eq (Get-FileHash -LiteralPath $file.FullName).Hash){continue}
    }
    $plans+=@{source=$file.FullName;target=$target;bytes=$bytes;name=$name;mask=[BitConverter]::ToUInt32($bytes,0x148)}
}
if(-not $plans.Count){Write-Output 'No header changes required.';return}
# Validate every input before applying any changes. Reject links in destination
# ancestors so a profile junction cannot redirect a write into the source data.
$ancestor=$headers
while($ancestor){
    if(Test-Path -LiteralPath $ancestor){
        if((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Header destination ancestor is a link: $ancestor"}
    }
    if($ancestor -eq $privateRoot){break}
    $ancestor=Split-Path -Parent $ancestor
}
$backup=Join-Path $privateRoot ('archive/dlc-headers-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
if($Apply){New-Item -ItemType Directory -Path $backup -Force|Out-Null;New-Item -ItemType Directory -Path $headers -Force|Out-Null}
foreach($plan in $plans){
    Write-Output ('{0}: preserve supplied mask {1:X8}; {2}' -f $plan.name,$plan.mask,$(if($Apply){'import'}else{'preview'}))
    if($Apply){
        if(Test-Path -LiteralPath $plan.target){Copy-Item -LiteralPath $plan.target -Destination (Join-Path $backup ($plan.name+'.header'))}
        [IO.File]::WriteAllBytes($plan.target,$plan.bytes)
    }
}
if($Apply){Write-Output "Imported $($plans.Count) headers. Earlier headers: $backup"}
