# Project Forza Plus 4 settings tab and contract shared by both launchers. Modified 2026-10-18.
# GPL-3.0-only. Preparation, verification and launch run in tools/forza_plus via ForzaPlus.Launch.ps1.

function Get-Pfp4Defaults {
    return @{
        enabled = $false
        source_folder = $null
        update_folder = $null
        prepared_game_folder = '.local/forza-plus/assets'
        mode = 'normal'
        physics_preset = 'stock'
        physics_target_fps = $null
        camera_fov = @{ follow_low=$null; follow_high=$null; hood=$null; bumper_high=$null; driver=$null }
        harder_ai_file = $null
    }
}

function Get-Pfp4Settings {
    [CmdletBinding()]
    param([Parameter(Mandatory)][System.Collections.IDictionary]$SettingsRoot)
    $result = Get-Pfp4Defaults
    $stored = if ($SettingsRoot.Contains('forza_plus')) { $SettingsRoot.forza_plus } else { $null }
    if ($stored -isnot [System.Collections.IDictionary]) { return $result }
    foreach ($key in $stored.Keys) {
        if ($key -eq 'camera_fov') { continue }
        $result[$key] = $stored[$key]
    }
    if ($stored.camera_fov -is [System.Collections.IDictionary]) {
        foreach ($key in $stored.camera_fov.Keys) { $result.camera_fov[$key] = $stored.camera_fov[$key] }
    }
    return $result
}

function Resolve-Pfp4EditionPath {
    param([Parameter(Mandatory)][string]$EditionRoot,[Parameter(Mandatory)][string]$Value)
    if ([IO.Path]::IsPathRooted($Value)) { return [IO.Path]::GetFullPath($Value) }
    return [IO.Path]::GetFullPath((Join-Path $EditionRoot $Value))
}

function Test-Pfp4PathOverlap {
    param([Parameter(Mandatory)][string]$Left,[Parameter(Mandatory)][string]$Right)
    $leftPath = [IO.Path]::GetFullPath($Left).TrimEnd('\','/')
    $rightPath = [IO.Path]::GetFullPath($Right).TrimEnd('\','/')
    return $leftPath.Equals($rightPath,[StringComparison]::OrdinalIgnoreCase) -or
        $leftPath.StartsWith($rightPath + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
        $rightPath.StartsWith($leftPath + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)
}

function Test-Pfp4ReparseAncestor {
    param([Parameter(Mandatory)][string]$Path)
    $cursor = [IO.DirectoryInfo]([IO.Path]::GetFullPath($Path))
    while ($null -ne $cursor) {
        if (Test-Path -LiteralPath $cursor.FullName) {
            $item = Get-Item -LiteralPath $cursor.FullName -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return $true }
        }
        $cursor = $cursor.Parent
    }
    return $false
}

function Get-Pfp4Sha256 {
    param([Parameter(Mandatory)][string]$Path)
    $stream=[IO.File]::OpenRead($Path)
    try {$sha=[Security.Cryptography.SHA256]::Create();try{return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}}
    finally {$stream.Dispose()}
}

function Sync-Pfp4WheelSettings {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$CommonWheelIni,
        [Parameter(Mandatory)][string]$ModeUserRoot,
        [Parameter(Mandatory)][ValidateSet('normal','hard','sandbox','vanilla')][string]$Mode,
        [Parameter(Mandatory)][string]$ReceiptPath,
        [Parameter(Mandatory)][bool]$AdmissionLeaseHeld,
        [Parameter(Mandatory)][bool]$GenerationReady
    )
    if (-not $AdmissionLeaseHeld -or -not $GenerationReady) { throw 'Wheel settings can only be copied when Forza Plus is ready and this launcher has exclusive access.' }
    $source=[IO.Path]::GetFullPath($CommonWheelIni);$destinationRoot=[IO.Path]::GetFullPath($ModeUserRoot);$receipt=[IO.Path]::GetFullPath($ReceiptPath)
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Shared wheel settings not found: $source" }
    if (-not (Test-Path -LiteralPath $destinationRoot -PathType Container)) { New-Item -ItemType Directory -Path $destinationRoot -Force|Out-Null }
    if (Test-Pfp4ReparseAncestor $destinationRoot) { throw 'The mode save folder cannot use links or junctions.' }
    $destination=Join-Path $destinationRoot 'wheel.ini'
    $sourceHash=Get-Pfp4Sha256 $source
    $previous=$null
    if (Test-Path -LiteralPath $receipt -PathType Leaf) {
        $previous=Get-Content -LiteralPath $receipt -Raw -Encoding UTF8|ConvertFrom-Json
        if ([IO.Path]::GetFullPath([string]$previous.destination) -ne $destination) { throw 'The wheel settings record points to a different mode folder.' }
    }
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $destinationHash=Get-Pfp4Sha256 $destination
        if ($previous -and $destinationHash -ne [string]$previous.last_managed_destination_sha256) {
            throw 'This mode''s wheel.ini has changed; back it up before copying shared settings.'
        }
        if (-not $previous -and $destinationHash -ne $sourceHash) {
            throw 'This mode already has its own wheel.ini; back it up before copying shared settings.'
        }
        if ($destinationHash -eq $sourceHash) { $changed=$false }
        else { $changed=$true }
    } else { $changed=$true }
    if ($changed) {
        $temp=$destination+'.tmp.'+[guid]::NewGuid().ToString('N')
        try {
            [IO.File]::Copy($source,$temp,$false)
            Move-Item -LiteralPath $temp -Destination $destination -Force
        } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue } }
    }
    $destinationHash=Get-Pfp4Sha256 $destination
    $record=[ordered]@{schema='fm4-pfp4-managed-wheel/1';mode=$Mode;source=$source;source_sha256=$sourceHash;destination=$destination;last_managed_destination_sha256=$destinationHash;updated_utc=[DateTime]::UtcNow.ToString('o')}
    $receiptParent=Split-Path -Parent $receipt;New-Item -ItemType Directory -Path $receiptParent -Force|Out-Null
    $receiptTemp=$receipt+'.tmp.'+[guid]::NewGuid().ToString('N')
    try {
        [IO.File]::WriteAllText($receiptTemp,($record|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $receiptTemp -Destination $receipt -Force
    } finally { if (Test-Path -LiteralPath $receiptTemp) { Remove-Item -LiteralPath $receiptTemp -Force -ErrorAction SilentlyContinue } }
    return [pscustomobject]@{changed=$changed;source_sha256=$sourceHash;destination_sha256=$destinationHash;destination=$destination;mode=$Mode}
}

function Get-Pfp4Decision {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary]$Settings,
        [Parameter(Mandatory)][string]$EditionRoot
    )
    if (-not $Settings.Contains('enabled') -or $Settings.enabled -isnot [bool]) {
        throw 'Forza Plus enabled must be true or false.'
    }
    $edition = [IO.Path]::GetFullPath($EditionRoot)
    if (-not $Settings.enabled) {
        return [pscustomobject]@{
            status='disabled'; enabled_requested=$false; launch_allowed=$false; native_hook_available=$false
            game_data_root=(Join-Path $edition '.local/game-disc1'); user_data_root=(Join-Path $edition '.local/user')
            receipt_id=$null; diagnostics=@()
        }
    }

    $sourceText = if ($null -eq $Settings.source_folder) { '' } else { ([string]$Settings.source_folder).Trim() }
    if (-not $sourceText) { throw 'Choose an extracted Forza Plus source folder.' }
    $source = Resolve-Pfp4EditionPath $edition $sourceText
    if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Forza Plus source folder not found: $source" }
    $updateText = if ($null -eq $Settings.update_folder) { '' } else { ([string]$Settings.update_folder).Trim() }
    $update = if ($updateText) { Resolve-Pfp4EditionPath $edition $updateText } else { $null }
    if ($update -and -not (Test-Path -LiteralPath $update -PathType Container)) { throw "Forza Plus update folder not found: $update" }
    $preparedText = if ($null -eq $Settings.prepared_game_folder) { '' } else { ([string]$Settings.prepared_game_folder).Trim() }
    if (-not $preparedText) { $preparedText = '.local/forza-plus/assets' }
    $prepared = Resolve-Pfp4EditionPath $edition $preparedText
    if ((Test-Pfp4PathOverlap $prepared $source) -or ($update -and (Test-Pfp4PathOverlap $prepared $update)) -or
        (Test-Pfp4PathOverlap $prepared (Join-Path $edition '.local/game-disc1')) -or
        (Test-Pfp4PathOverlap $prepared (Join-Path $edition '.local/user'))) {
        throw 'Keep the prepared copy separate from game, save, source and update folders.'
    }
    foreach ($path in @($source,$update,$prepared) | Where-Object { $_ }) {
        if (Test-Pfp4ReparseAncestor $path) { throw "Forza Plus folders cannot use links or junctions: $path" }
    }
    $mode = [string]$Settings.mode
    if ($mode -notin @('normal','hard','sandbox','vanilla')) { throw 'Choose Normal, Hard, Sandbox or Vanilla mode.' }
    $preset = [string]$Settings.physics_preset
    if ($preset -notin @('stock','100','120','144','165','custom')) { throw 'Choose Mod default, 100, 120, 144, 165 or Custom physics.' }
    $target = $null; $updateHz = $null
    if ($preset -eq 'custom') {
        $parsed = 0.0
        if (-not [double]::TryParse([string]$Settings.physics_target_fps,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$parsed) -or
            [double]::IsNaN($parsed) -or [double]::IsInfinity($parsed) -or $parsed -lt 1 -or $parsed -gt 1000) {
            throw 'Custom target FPS must be between 1 and 1000.'
        }
        $target = $parsed
        $updateHz = [int][Math]::Floor(($parsed * 5.79) + 0.5)
    }
    if ($Settings.camera_fov -isnot [System.Collections.IDictionary]) { throw 'Invalid camera FOV settings.' }
    foreach ($field in @('follow_low','follow_high','hood','bumper_high','driver')) {
        if (-not $Settings.camera_fov.Contains($field) -or $null -eq $Settings.camera_fov[$field]) { continue }
        $fov = 0.0
        if (-not [double]::TryParse([string]$Settings.camera_fov[$field],[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$fov) -or
            [double]::IsNaN($fov) -or [double]::IsInfinity($fov) -or $fov -le 0 -or $fov -ge 180) {
            throw "Camera FOV for $field must be strictly between 0 and 180 degrees."
        }
    }
    $aiText = if ($null -eq $Settings.harder_ai_file) { '' } else { ([string]$Settings.harder_ai_file).Trim() }
    if ($aiText) {
        $aiPath = Resolve-Pfp4EditionPath $edition $aiText
        if (-not (Test-Path -LiteralPath $aiPath -PathType Leaf)) { throw "Harder opponents file not found: $aiPath" }
        throw 'Harder opponent files are unsupported.'
    }
    return [pscustomobject]@{
        status='options_valid'; enabled_requested=$true; launch_allowed=$false; native_hook_available=$false
        game_data_root=$prepared; user_data_root=(Join-Path $edition ('.local/user/.fm4-plus/' + $mode))
        receipt_id=$null; mode=$mode; physics_preset=$preset; physics_target_fps=$target
        car_physics_update_hz=$updateHz; source_folder=$source; update_folder=$update; prepared_game_folder=$prepared
        diagnostics=@('Ready to prepare Forza Plus; the first launch may take several minutes.')
    }
}

function New-Pfp4SettingsTab {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$TabControl,
        [Parameter(Mandatory)][System.Collections.IDictionary]$SettingsRoot,
        [Parameter(Mandatory)][string]$EditionRoot
    )
    $settings = Get-Pfp4Settings $SettingsRoot
    $tab = [System.Windows.Forms.TabPage]::new('Forza Plus 4')
    $tab.AutoScroll = $true
    [void]$TabControl.TabPages.Add($tab)
    $ui = @{ Tab=$tab; EditionRoot=[IO.Path]::GetFullPath($EditionRoot) }
    function Add-PfpLabel($parent,[string]$text,[int]$x,[int]$y,[int]$width=260) {
        $control=[System.Windows.Forms.Label]::new();$control.Text=$text;$control.SetBounds($x,$y,$width,26);$parent.Controls.Add($control);return $control
    }
    function Add-PfpTextRow($parent,[string]$label,[int]$y,[string]$value,[string]$buttonText,[scriptblock]$browse) {
        $null=Add-PfpLabel $parent $label 20 $y 230
        $field=[System.Windows.Forms.TextBox]::new();$field.Text=$value;$field.SetBounds(250,$y,330,27);$parent.Controls.Add($field)
        $button=[System.Windows.Forms.Button]::new();$button.Text=$buttonText;$button.SetBounds(590,$y,95,29);$parent.Controls.Add($button);$button.Add_Click($browse.GetNewClosure())
        return @{Text=$field;Button=$button}
    }
    $ui.Enabled=[System.Windows.Forms.CheckBox]::new();$ui.Enabled.Text='Enable Project Forza Plus 4';$ui.Enabled.Checked=($settings.enabled -is [bool] -and $settings.enabled);$ui.Enabled.SetBounds(20,16,430,28);$tab.Controls.Add($ui.Enabled)
    $ui.Status=[System.Windows.Forms.Label]::new();$ui.Status.Text=$(if($ui.Enabled.Checked){'Enabled; uses a separate game copy and saves.'}else{'Off; uses the original game and saves.'});$ui.Status.SetBounds(20,48,660,40);$ui.Status.ForeColor=[Drawing.Color]::DarkSlateBlue;$tab.Controls.Add($ui.Status)
    $folderBrowse={param($textBox)$dialog=[Windows.Forms.FolderBrowserDialog]::new();try{if($dialog.ShowDialog() -eq 'OK'){$textBox.Text=$dialog.SelectedPath}}finally{$dialog.Dispose()}}
    $ui.Source=Add-PfpTextRow $tab 'Extracted mod folder' 96 ([string]$settings.source_folder) 'Browse…' { $folderBrowse.Invoke($ui.Source.Text) }
    $ui.Prepared=Add-PfpTextRow $tab 'Prepared copy folder' 136 ([string]$settings.prepared_game_folder) 'Browse…' { $folderBrowse.Invoke($ui.Prepared.Text) }
    $ui.Update=Add-PfpTextRow $tab 'Optional update folder' 176 ([string]$settings.update_folder) 'Browse…' { $folderBrowse.Invoke($ui.Update.Text) }
    $ui.Mode=[Windows.Forms.ComboBox]::new();$ui.Mode.DropDownStyle='DropDownList';$ui.Mode.Items.AddRange(@('Normal','Hard','Sandbox','Vanilla'));$ui.Mode.SetBounds(250,216,250,27);$tab.Controls.Add($ui.Mode)
    $modeValues=@('normal','hard','sandbox','vanilla');$modeIndex=[Array]::IndexOf($modeValues,[string]$settings.mode);if($modeIndex -lt 0){$modeIndex=0};$ui.Mode.SelectedIndex=$modeIndex
    $null=Add-PfpLabel $tab 'Career mode' 20 216 230
    $ui.Physics=[Windows.Forms.ComboBox]::new();$ui.Physics.DropDownStyle='DropDownList';$ui.Physics.Items.AddRange(@('Mod default','100 FPS','120 FPS','144 FPS','165 FPS','Custom'));$ui.Physics.SetBounds(250,256,354,27);$tab.Controls.Add($ui.Physics)
    $physicsValues=@('stock','100','120','144','165','custom');$physicsIndex=[Array]::IndexOf($physicsValues,[string]$settings.physics_preset);if($physicsIndex -lt 0){$physicsIndex=0};$ui.Physics.SelectedIndex=$physicsIndex;$null=Add-PfpLabel $tab 'Physics preset' 20 256 230
    $null=Add-PfpLabel $tab 'Custom target FPS (1–1000)' 20 296 350
    $ui.TargetFps=[System.Windows.Forms.NumericUpDown]::new();$ui.TargetFps.Minimum=1;$ui.TargetFps.Maximum=1000;$ui.TargetFps.DecimalPlaces=1;$ui.TargetFps.Increment=1;$ui.TargetFps.SetBounds(390,296,130,27)
    $targetValue=60.0;try{if($null -ne $settings.physics_target_fps){$targetValue=[double]$settings.physics_target_fps}}catch{};$ui.TargetFps.Value=[Math]::Min(1000,[Math]::Max(1,[decimal]$targetValue));$tab.Controls.Add($ui.TargetFps)
    $null=Add-PfpLabel $tab 'Match the physics target to your intended frame rate.' 20 326 660
    $null=Add-PfpLabel $tab 'Camera field of view (degrees)' 20 362 660
    $camera=@();$cameraSpecs=@(@('follow_low','Follow low',54),@('follow_high','Follow high',54),@('hood','Hood',66),@('bumper_high','High bumper',53),@('driver','Cockpit',62))
    $y=390
    foreach($spec in $cameraSpecs){
        $key=$spec[0];$null=Add-PfpLabel $tab $spec[1] 20 $y 250
        $inherit=[System.Windows.Forms.CheckBox]::new();$inherit.Text='Mod default';$inherit.SetBounds(280,$y,105,25);$value=$settings.camera_fov[$key];$inherit.Checked=($null -eq $value);$tab.Controls.Add($inherit)
        $number=[System.Windows.Forms.NumericUpDown]::new();$number.Minimum=[decimal]0.1;$number.Maximum=[decimal]179.9;$number.DecimalPlaces=1;$number.Increment=[decimal]1;$number.SetBounds(400,$y,120,27);$n=[decimal]$spec[2];try{if($null -ne $value){$n=[decimal]$value}}catch{};$number.Value=[Math]::Min($number.Maximum,[Math]::Max($number.Minimum,$n));$number.Enabled=(-not $inherit.Checked);$inherit.Add_CheckedChanged({$number.Enabled=(-not $inherit.Checked)}.GetNewClosure());$tab.Controls.Add($number)
        $camera+=@{Key=$key;Inherit=$inherit;Number=$number};$y+=36
    }
    $ui.Camera=$camera
    $aiBrowse={param($textBox)$dialog=[Windows.Forms.OpenFileDialog]::new();$dialog.Filter='INI files (*.ini)|*.ini|All files (*.*)|*.*';try{if($dialog.ShowDialog() -eq 'OK'){$textBox.Text=$dialog.FileName}}finally{$dialog.Dispose()}}
    $ui.HarderAi=Add-PfpTextRow $tab 'Harder opponents file' 576 ([string]$settings.harder_ai_file) 'Browse…' { $aiBrowse.Invoke($ui.HarderAi.Text) }
    $null=Add-PfpLabel $tab 'Unsupported; leave this empty.' 20 608 660
    $ui.CheckStatus=[System.Windows.Forms.Button]::new();$ui.CheckStatus.Text='Check status';$ui.CheckStatus.SetBounds(20,648,180,32);$tab.Controls.Add($ui.CheckStatus)
    $ui.CheckStatus.Add_Click({
        try {
            $candidate=Read-Pfp4Controls -Controls $ui -BaseSettings $SettingsRoot -EditionRoot $EditionRoot
            $decision=Get-Pfp4Decision -Settings $candidate -EditionRoot $EditionRoot
            if($decision.status -eq 'disabled'){$ui.Status.Text='Off; uses the original game and saves.'}
            elseif(Get-Command Invoke-Pfp4Backend -ErrorAction SilentlyContinue){
                $ui.Status.Text='Checking Forza Plus…';$ui.Tab.Refresh()
                $backend=Join-Path $EditionRoot 'tools/forza_plus/backend_cli.py'
                $game=Join-Path $EditionRoot '.local/work/fm4/out/build/win-amd64-release/fm4.exe'
                $response=Invoke-Pfp4Backend -Operation inspect -EditionRoot $EditionRoot -Settings $candidate -BackendCliPath $backend -GameExecutable $game
                $ui.Status.Text=Get-Pfp4StatusText $response
            }
            else{$ui.Status.Text=($decision.diagnostics -join ' ')}
        }
        catch {$ui.Status.Text='Cannot check status: '+$_.Exception.Message}
    }.GetNewClosure())
    return $ui
}

function Get-Pfp4StatusText {
    param([Parameter(Mandatory)]$Response)
    switch([string]$Response.status){
        'ready' {
            $eligible=@($Response.eligible_content_packages).Count;$omitted=@($Response.omitted_content_packages).Count
            return "Ready: $eligible content packs included, $omitted skipped."
        }
        'prepared_missing' {return 'First launch prepares Forza Plus and may take several minutes.'}
        'options_changed' {return 'Options changed; the next launch prepares a new copy.'}
        'native_hook_unavailable' {return 'Forza Plus is unavailable in this build.'}
        'disabled' {return 'Off; uses the original game and saves.'}
        default {return 'Status: '+[string]$Response.status}
    }
}

function Read-Pfp4Controls {
    [CmdletBinding()]
    param([Parameter(Mandatory)][hashtable]$Controls,[Parameter(Mandatory)][System.Collections.IDictionary]$BaseSettings,[Parameter(Mandatory)][string]$EditionRoot)
    $result = Get-Pfp4Settings $BaseSettings
    $result.enabled = [bool]$Controls.Enabled.Checked
    $result.source_folder = if ([string]::IsNullOrWhiteSpace($Controls.Source.Text.Text)) { $null } else { $Controls.Source.Text.Text.Trim() }
    $result.prepared_game_folder = if ([string]::IsNullOrWhiteSpace($Controls.Prepared.Text.Text)) { '.local/forza-plus/assets' } else { $Controls.Prepared.Text.Text.Trim() }
    $result.update_folder = if ([string]::IsNullOrWhiteSpace($Controls.Update.Text.Text)) { $null } else { $Controls.Update.Text.Text.Trim() }
    $result.mode = @('normal','hard','sandbox','vanilla')[[Math]::Max(0,$Controls.Mode.SelectedIndex)]
    $result.physics_preset = @('stock','100','120','144','165','custom')[[Math]::Max(0,$Controls.Physics.SelectedIndex)]
    $result.physics_target_fps = if ($result.physics_preset -eq 'custom') { [double]$Controls.TargetFps.Value } else { $null }
    foreach($entry in $Controls.Camera){$result.camera_fov[$entry.Key]=if($entry.Inherit.Checked){$null}else{[double]$entry.Number.Value}}
    $result.harder_ai_file = if ([string]::IsNullOrWhiteSpace($Controls.HarderAi.Text.Text)) { $null } else { $Controls.HarderAi.Text.Text.Trim() }
    return $result
}

function Test-Pfp4ProposalContract {
    $testRoot=Join-Path ([IO.Path]::GetTempPath()) ('fm4-pfp4-contract-'+[guid]::NewGuid().ToString('N'))
    $resolvedTestRoot=[IO.Path]::GetFullPath($testRoot)
    $tempPrefix=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if(-not $resolvedTestRoot.StartsWith($tempPrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing PFP4 contract fixture outside the temp directory.'}
    try {
        $source=Join-Path $testRoot 'source';$update=Join-Path $testRoot 'update';$ai=Join-Path $testRoot 'harder.ini'
        New-Item -ItemType Directory -Path $source,$update -Force|Out-Null;[IO.File]::WriteAllText($ai,'fixture only')
        $settings=Get-Pfp4Defaults;$settings.enabled=$false;$settings.source_folder='missing source';$settings.mode='stale-invalid';$settings.physics_preset='stale-invalid';$settings.camera_fov=$null;$settings.harder_ai_file='missing.ini'
        $decision=Get-Pfp4Decision -Settings $settings -EditionRoot $testRoot
        if($decision.status -ne 'disabled' -or $decision.game_data_root -ne (Join-Path $testRoot '.local/game-disc1') -or $decision.user_data_root -ne (Join-Path $testRoot '.local/user')){throw 'Disabled PFP4 settings did not take the stock route before stale-field validation.'}

        $settings=Get-Pfp4Defaults;$settings.enabled=$true;$settings.source_folder='source';$settings.update_folder='update';$settings.mode='hard';$settings.physics_preset='custom';$settings.physics_target_fps=120
        $settings.camera_fov.follow_low=$null;$settings.camera_fov.follow_high=[double]0.1;$settings.camera_fov.hood=[double]179.9;$settings.camera_fov.bumper_high=$null;$settings.camera_fov.driver=62
        $decision=Get-Pfp4Decision -Settings $settings -EditionRoot $testRoot
        if($decision.status -ne 'options_valid' -or $decision.launch_allowed -or $decision.native_hook_available -or
            $decision.game_data_root -ne (Join-Path $testRoot '.local/forza-plus/assets') -or
            $decision.user_data_root -ne (Join-Path $testRoot '.local/user/.fm4-plus/hard') -or
            $decision.car_physics_update_hz -ne 695 -or $decision.source_folder -ne [IO.Path]::GetFullPath($source) -or
            $decision.update_folder -ne [IO.Path]::GetFullPath($update)) {throw 'Enabled PFP4 contract/root/FPS gate differs from the private native contract.'}

        foreach($preset in @('100','120','144','165')){$settings.physics_preset=$preset;$decision=Get-Pfp4Decision -Settings $settings -EditionRoot $testRoot;if($decision.physics_preset -ne $preset -or $decision.launch_allowed){throw "Supplied physics preset mapping failed for $preset."}}
        $settings.physics_preset='stock';$decision=Get-Pfp4Decision -Settings $settings -EditionRoot $testRoot;if($decision.physics_target_fps -ne $null){throw 'Stock physics did not inherit the selected source default.'}
        $settings.physics_preset='custom';$settings.physics_target_fps=1000;$null=Get-Pfp4Decision -Settings $settings -EditionRoot $testRoot

        $cases=@(
            @{Name='custom FPS below minimum';Mutate={param($s)$s.physics_preset='custom';$s.physics_target_fps=0};Pattern='between 1 and 1000'},
            @{Name='custom FPS above maximum';Mutate={param($s)$s.physics_preset='custom';$s.physics_target_fps=1001};Pattern='between 1 and 1000'},
            @{Name='FOV zero';Mutate={param($s)$s.camera_fov.driver=0};Pattern='strictly between 0 and 180'},
            @{Name='FOV 180';Mutate={param($s)$s.camera_fov.driver=180};Pattern='strictly between 0 and 180'},
            @{Name='prepared/source overlap';Mutate={param($s)$s.prepared_game_folder='source'};Pattern='separate from game'},
            @{Name='unmapped AI variant';Mutate={param($s)$s.harder_ai_file='harder.ini'};Pattern='Harder opponent files are unsupported'}
        )
        foreach($case in $cases){
            $copy=Get-Pfp4Defaults;$copy.enabled=$true;$copy.source_folder='source';$copy.update_folder='update';$copy.mode='normal';$copy.physics_preset='stock';$copy.camera_fov.driver=62
            & $case.Mutate $copy
            $failed=$false;try{$null=Get-Pfp4Decision -Settings $copy -EditionRoot $testRoot}catch{$failed=$_.Exception.Message -like ('*'+$case.Pattern+'*')}
            if(-not $failed){throw "PFP4 invalid contract case did not fail safely: $($case.Name)."}
        }
        $common=Join-Path $resolvedTestRoot 'common/wheel.ini';$modeRoot=Join-Path $resolvedTestRoot '.local/user/.fm4-plus/hard';$receipt=Join-Path $resolvedTestRoot '.local/forza-plus/managed-wheel-hard.json'
        New-Item -ItemType Directory -Path (Split-Path -Parent $common),$modeRoot -Force|Out-Null
        $commonText="[wheel]`nenabled=1`ndevice=Test wheel`n[custom]`nkeep=1`n";[IO.File]::WriteAllText($common,$commonText,[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $modeRoot 'profile.bin'),'profile untouched',[Text.Encoding]::ASCII)
        $leaseRejected=$false;try{$null=Sync-Pfp4WheelSettings -CommonWheelIni $common -ModeUserRoot $modeRoot -Mode hard -ReceiptPath $receipt -AdmissionLeaseHeld $false -GenerationReady $true}catch{$leaseRejected=$_.Exception.Message -like '*only be copied when Forza Plus is ready*'}
        if(-not $leaseRejected -or (Test-Path -LiteralPath (Join-Path $modeRoot 'wheel.ini'))){throw 'Mode wheel settings were written without the edition admission/launch lease.'}
        $first=Sync-Pfp4WheelSettings -CommonWheelIni $common -ModeUserRoot $modeRoot -Mode hard -ReceiptPath $receipt -AdmissionLeaseHeld $true -GenerationReady $true
        if(-not $first.changed -or [IO.File]::ReadAllText($first.destination) -cne $commonText -or -not (Test-Path -LiteralPath (Join-Path $modeRoot 'profile.bin'))){throw 'Mode wheel sync did not copy exact wheel.ini bytes while preserving separate profile data.'}
        [IO.File]::WriteAllText($common,$commonText.Replace('device=Test wheel','device=Changed wheel'),[Text.UTF8Encoding]::new($false))
        $second=Sync-Pfp4WheelSettings -CommonWheelIni $common -ModeUserRoot $modeRoot -Mode hard -ReceiptPath $receipt -AdmissionLeaseHeld $true -GenerationReady $true
        if(-not $second.changed -or [IO.File]::ReadAllText($second.destination) -notmatch 'device=Changed wheel'){throw 'Managed wheel settings did not refresh from the shared source.'}
        [IO.File]::AppendAllText($second.destination,"`n[user-edit]`nkeep=1`n",[Text.UTF8Encoding]::new($false));$edited=[IO.File]::ReadAllBytes($second.destination)
        $editRejected=$false;try{$null=Sync-Pfp4WheelSettings -CommonWheelIni $common -ModeUserRoot $modeRoot -Mode hard -ReceiptPath $receipt -AdmissionLeaseHeld $true -GenerationReady $true}catch{$editRejected=$_.Exception.Message -like '*wheel.ini has changed*'}
        if(-not $editRejected -or [Convert]::ToBase64String([IO.File]::ReadAllBytes($second.destination)) -ne [Convert]::ToBase64String($edited)){throw 'An independent mode wheel.ini edit was overwritten.'}
        Write-Output 'PASS: PFP4 stock-disabled gate, edition-root paths, physics/FOV contract, unsupported AI, blocked launch, and lease-gated mode wheel.ini synchronization preserving independent edits.'
    } finally {
        if(Test-Path -LiteralPath $resolvedTestRoot -PathType Container){Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force}
    }
}
