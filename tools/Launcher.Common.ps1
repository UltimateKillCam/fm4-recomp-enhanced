# Windows PowerShell 5.1 / PowerShell 7 launcher support. Modified 2026-10-17.
# GPL-3.0-only.

function Write-Fm4Utf8File {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory, ValueFromPipeline)][AllowEmptyString()][string]$Text
    )
    process {
        $resolved = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
        [IO.File]::WriteAllText($resolved, $Text, [Text.UTF8Encoding]::new($false))
    }
}

function ConvertFrom-Fm4Json {
    param([Parameter(Mandatory, ValueFromPipeline)][string]$Json)
    process {
        function Convert-JsonValue($value) {
            if ($null -eq $value) { return $null }
            if ($value -is [Management.Automation.PSCustomObject]) {
                $table = @{}
                foreach ($property in $value.PSObject.Properties) {
                    $table[$property.Name] = Convert-JsonValue $property.Value
                }
                return $table
            }
            if ($value -is [Array]) {
                $items = @(
                    foreach ($item in $value) { ,(Convert-JsonValue $item) }
                )
                return ,$items
            }
            return $value
        }
        Convert-JsonValue (ConvertFrom-Json -InputObject $Json)
    }
}

function Test-Fm4Finite([double]$Value) {
    return -not ([double]::IsNaN($Value) -or [double]::IsInfinity($Value))
}

function ConvertTo-Fm4WindowsArgument {
    param([AllowEmptyString()][string]$Argument)
    # Microsoft CRT quoting, including empty arguments, embedded quotes and
    # trailing backslashes. This is a process command line, never shell code.
    $quoted = [Text.StringBuilder]::new()
    [void]$quoted.Append('"')
    $slashes = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq '\') {
            $slashes++
        } elseif ($character -eq '"') {
            [void]$quoted.Append(('\' * (2 * $slashes + 1)))
            [void]$quoted.Append('"')
            $slashes = 0
        } else {
            [void]$quoted.Append(('\' * $slashes))
            [void]$quoted.Append($character)
            $slashes = 0
        }
    }
    [void]$quoted.Append(('\' * (2 * $slashes)))
    [void]$quoted.Append('"')
    return $quoted.ToString()
}

function Set-Fm4ProcessArguments {
    param(
        [Parameter(Mandatory)][Diagnostics.ProcessStartInfo]$StartInfo,
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$ArgumentList
    )
    $StartInfo.Arguments = (@($ArgumentList | ForEach-Object {
        ConvertTo-Fm4WindowsArgument $_
    }) -join ' ')
}

function Get-Fm4PowerShellExecutable {
    foreach ($name in @('pwsh.exe', 'powershell.exe')) {
        $candidate = Join-Path $PSHOME $name
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    throw 'Cannot find the PowerShell executable.'
}
