# Private launcher bridge helper. Python backend is stdlib-only; request values
# are JSON files and never become PowerShell or shell code.
function Resolve-Pfp4Python {
    param([Parameter(Mandatory)][string]$EditionRoot)
    $venv=Join-Path $EditionRoot '.local/forza-plus/python-venv/Scripts/python.exe'
    if(Test-Path -LiteralPath $venv -PathType Leaf){$executable=$venv;$prefix=@()}
    else {
        $py=Get-Command py.exe -ErrorAction SilentlyContinue
        if($py){$executable=$py.Source;$prefix=@('-3')}
        else{$python=Get-Command python.exe -ErrorAction SilentlyContinue;if(-not $python){throw 'Python 3.10 or newer is required for Forza Plus preparation. Install Python or provide the private .local/forza-plus/python-venv.'};$executable=$python.Source;$prefix=@()}
    }
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$executable;$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $versionArgs=@($prefix)+@('--version')
    $info.Arguments=(@($versionArgs|ForEach-Object{if($_ -eq '-3'){'-3'}else{ConvertTo-Fm4WindowsArgument $_}})-join ' ')
    $process=[Diagnostics.Process]::Start($info)
    try{$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();$process.WaitForExit();$versionText=$stdout.GetAwaiter().GetResult()+' '+$stderr.GetAwaiter().GetResult();if($process.ExitCode -ne 0){throw "Python version check failed: $versionText"}}finally{$process.Dispose()}
    $match=[regex]::Match($versionText,'Python\s+(\d+)\.(\d+)')
    if(-not $match.Success -or [int]$match.Groups[1].Value -lt 3 -or ([int]$match.Groups[1].Value -eq 3 -and [int]$match.Groups[2].Value -lt 10)){throw "Python 3.10 or newer is required; found: $versionText"}
    return [pscustomobject]@{Executable=$executable;Prefix=$prefix;Version=$match.Value}
}

function Invoke-Pfp4Backend {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet('inspect','prepare','verify','select_stock','launch')][string]$Operation,
        [Parameter(Mandatory)][string]$EditionRoot,
        [Parameter(Mandatory)][System.Collections.IDictionary]$Settings,
        [Parameter(Mandatory)][string]$BackendCliPath,
        [string]$GameExecutable,
        [string[]]$GameArguments=@(),
        [System.Collections.IDictionary]$GameEnvironment=@{},
        [string[]]$GameEnvironmentRemove=@(),
        [switch]$WaitForExit
    )
    $edition=[IO.Path]::GetFullPath($EditionRoot);$backend=[IO.Path]::GetFullPath($BackendCliPath)
    if(-not(Test-Path -LiteralPath $backend -PathType Leaf)){throw "Forza Plus backend CLI is missing: $backend"}
    $request=[ordered]@{protocol='fm4-plus-proposal/1';operation=$Operation;edition_root=$edition;settings=$Settings;game_executable=$GameExecutable;game_arguments=@($GameArguments);game_environment=$GameEnvironment;game_environment_remove=@($GameEnvironmentRemove);wait_for_exit=[bool]$WaitForExit}
    if($Operation -eq 'launch' -and (-not $GameExecutable -or -not(Test-Path -LiteralPath $GameExecutable -PathType Leaf))){throw 'Forza Plus launch requires the selected game executable.'}
    $python=Resolve-Pfp4Python -EditionRoot $edition
    $temp=Join-Path ([IO.Path]::GetTempPath()) ('fm4-pfp4-request-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $temp -Force|Out-Null
    $requestPath=Join-Path $temp 'request.json';$responsePath=Join-Path $temp 'response.json'
    try{
        Write-Fm4Utf8File -Path $requestPath -Text ($request|ConvertTo-Json -Depth 20)
        $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$python.Executable;$info.WorkingDirectory=(Split-Path -Parent $backend);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        $arguments=@($python.Prefix)+@($backend,'--request',$requestPath,'--response',$responsePath)
        $info.Arguments=(@($arguments|ForEach-Object{if($_ -eq '-3'){'-3'}else{ConvertTo-Fm4WindowsArgument $_}})-join ' ')
        $process=[Diagnostics.Process]::Start($info)
        try{$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(-not $process.WaitForExit(900000)){$process.Kill();$process.WaitForExit();throw 'Forza Plus backend exceeded the fifteen-minute request limit.'};$exitCode=$process.ExitCode;$standardOutput=$stdout.GetAwaiter().GetResult();$standardError=$stderr.GetAwaiter().GetResult()}finally{$process.Dispose()}
        if(-not(Test-Path -LiteralPath $responsePath -PathType Leaf)){throw "Forza Plus backend did not write a response (exit $exitCode). $standardError $standardOutput"}
        $response=Get-Content -LiteralPath $responsePath -Raw -Encoding UTF8|ConvertFrom-Fm4Json
        if($response.protocol -ne 'fm4-plus-proposal/1'){throw 'Forza Plus backend response protocol does not match.'}
        if($exitCode -ne 0 -or $response.status -eq 'failed'){$message=if($response.diagnostics.Count){($response.diagnostics|ForEach-Object{$_.message}) -join '; '}else{$standardError+' '+$standardOutput};throw "Forza Plus $Operation failed: $message"}
        return $response
    }finally{
        $resolved=[IO.Path]::GetFullPath($temp);$prefix=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
        if(-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing request cleanup outside the temporary directory.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Test-Pfp4LaunchBridge {
    $python=Resolve-Pfp4Python -EditionRoot ([IO.Path]::GetTempPath())
    $fixture=Join-Path ([IO.Path]::GetTempPath()) ('fm4-pfp4-bridge-test-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $fixture -Force|Out-Null
    try{
        $mock=Join-Path $fixture 'mock_backend.py'
        $code=@'
from pathlib import Path
import argparse,json,sys
p=argparse.ArgumentParser();p.add_argument('--request',required=True);p.add_argument('--response');a=p.parse_args()
r=json.loads(Path(a.request).read_text(encoding='utf-8-sig'))
result={'protocol':'fm4-plus-proposal/1','status':'ready' if r['settings'].get('enabled') else 'disabled','launch_allowed':False,'echo_argument':r.get('game_arguments',[]),'environment':r.get('game_environment',{})}
if a.response:Path(a.response).write_text(json.dumps(result),encoding='utf-8')
print(json.dumps(result));print('mock harmless stderr',file=sys.stderr)
'@
        [IO.File]::WriteAllText($mock,$code,[Text.UTF8Encoding]::new($true))
        $settings=@{enabled=$true;source_folder='C:\Fixture with spaces\source';camera_fov=@{driver=$null}}
        $result=Invoke-Pfp4Backend -Operation inspect -EditionRoot $fixture -Settings $settings -BackendCliPath $mock -GameArguments @('arg with spaces','literal; text') -GameEnvironment @{FM4_CAPTURE_DIR='C:\captures\one'}
        if($result.status -ne 'ready' -or $result.echo_argument.Count -ne 2 -or $result.echo_argument[0] -ne 'arg with spaces' -or $result.echo_argument[1] -ne 'literal; text' -or $result.environment.FM4_CAPTURE_DIR -ne 'C:\captures\one'){throw 'PFP4 JSON request/response quoting or environment boundary failed.'}
        Write-Output 'PASS: Python 3.10+ resolution, UTF-8 JSON request/response, spaces and shell punctuation remain data, stdout/stderr handled separately.'
    }finally{$resolved=[IO.Path]::GetFullPath($fixture);$prefix=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\';if(-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing bridge test cleanup outside the temporary directory.'};Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue}
}
