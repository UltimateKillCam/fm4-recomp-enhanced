[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourcePackages,
      [Parameter(Mandatory)][string]$UserDataRoot,[switch]$Apply)
$ErrorActionPreference='Stop'
if($Apply){
    foreach($game in Get-CimInstance Win32_Process -Filter "Name='fm4.exe'"){
        if(-not $game.CommandLine -or $game.CommandLine.Replace('/','\').IndexOf(
            [IO.Path]::GetFullPath($UserDataRoot),[StringComparison]::OrdinalIgnoreCase) -ge 0){
            throw 'Close the game using this profile before installing original content.'
        }
    }
}
$arguments=@((Join-Path $PSScriptRoot 'Dlc-Packages.py'),'--source',$SourcePackages,'--user-root',$UserDataRoot)
if($Apply){$arguments+='--apply'}
& python @arguments
if($LASTEXITCODE -ne 0){throw 'Original content validation or copy failed.'}
