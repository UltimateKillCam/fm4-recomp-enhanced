[CmdletBinding()]
param([switch]$SelfTest,[string]$PreviewDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Launcher.Common.ps1')
. (Join-Path $PSScriptRoot 'Launcher.Ui.ps1')
. (Join-Path $PSScriptRoot 'ForzaPlus.Common.ps1')
. (Join-Path $PSScriptRoot 'ForzaPlus.Launch.ps1')
. (Join-Path $PSScriptRoot 'Neural.Common.ps1')
if($PreviewDirectory -and -not $SelfTest){throw 'PreviewDirectory requires SelfTest; previews never display a window.'}
$root=if($env:FM4_SETTINGS_TEST_ROOT){[IO.Path]::GetFullPath($env:FM4_SETTINGS_TEST_ROOT)}else{[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))}
$settingsPath=Join-Path $root '.local/enhancements/settings.json'
$defaults=Get-Content (Join-Path $root 'config/enhanced-settings.json') -Raw -Encoding UTF8 | ConvertFrom-Fm4Json
$settings=$defaults.Clone()
$current=@{}
if(Test-Path -LiteralPath $settingsPath){
    $current=Get-Content -LiteralPath $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Fm4Json
    foreach($key in $current.Keys){$settings[$key]=$current[$key]}
}
$settings.forza_plus=Get-Pfp4Settings $settings
$wheelPath=Join-Path $root '.local/user/wheel.ini'
$wheelDefaults="[wheel]`nenabled=1`n[force]`nenabled=1`ngain=0.4`n[controller]`nadaptive_triggers=1`nadaptive_gain=0.35`nrumble_gain=1`n"
$wheelText=if(Test-Path -LiteralPath $wheelPath){Get-Content -LiteralPath $wheelPath -Raw -Encoding UTF8}else{$wheelDefaults}
function Get-Ini([string]$section,[string]$key,[string]$fallback){
    $block=[regex]::Match($script:wheelText,'(?ms)^\['+[regex]::Escape($section)+'\]\s*\r?\n(.*?)(?=^\[|\z)')
    if($block.Success){$match=[regex]::Match($block.Groups[1].Value,'(?m)^'+[regex]::Escape($key)+'[ \t]*=[ \t]*([^\r\n]*)');if($match.Success){return $match.Groups[1].Value.Trim()}}
    return $fallback
}
function Set-Ini([string]$text,[string]$section,[string]$key,[string]$value){
    if($value -match '[\r\n]'){throw 'Wheel settings cannot contain line breaks.'}
    $pattern='(?ms)^\['+[regex]::Escape($section)+'\]\s*\r?\n(.*?)(?=^\[|\z)'
    $block=[regex]::Match($text,$pattern)
    if(-not $block.Success){return $text.TrimEnd()+"`n`n[$section]`n$key=$value`n"}
    $body=$block.Groups[1].Value
    $entry='(?m)^'+[regex]::Escape($key)+'[ \t]*=[ \t]*[^\r\n]*'
    if([regex]::IsMatch($body,$entry)){$body=[regex]::Replace($body,$entry,[Text.RegularExpressions.MatchEvaluator]{param($m) "$key=$value"}.GetNewClosure())}
    else{$body=$body.TrimEnd()+"`n$key=$value`n`n"}
    return $text.Substring(0,$block.Groups[1].Index)+$body+$text.Substring($block.Groups[1].Index+$block.Groups[1].Length)
}
[Windows.Forms.Application]::EnableVisualStyles()
$shell=New-Fm4Shell -Title 'FM4 Enhanced Settings' -Subtitle 'Enhanced settings' -Badge 'Enhanced' -Height 780
$form=$shell.Form;$tabs=$shell.Pages
# Page layout: section titles start at x=20; option labels at x=32; fields at x=290.
$fieldX=290;$fieldWidth=470
$rowLabels=@{};$rowHints=@{}
function New-Tab([string]$title){$tab=[Windows.Forms.TabPage]::new($title);$tabs.TabPages.Add($tab);return $tab}
function Label($parent,[string]$text,[int]$x,[int]$y,[int]$width=690){$c=[Windows.Forms.Label]::new();$c.Text=$text;$c.SetBounds($x,$y+4,$width,24);$parent.Controls.Add($c);return $c}
function Hint($parent,$control,[string]$text,[int]$y,[int]$x=32){$h=Add-Fm4Hint $parent $text $x $y (790-$x);if($control){$script:rowHints[$control]=$h};return $h}
function Choice($parent,[string]$label,[int]$y,[object[]]$items,[int]$selected,[int]$x=$fieldX,[int]$width=$fieldWidth,[int]$labelX=32,[int]$labelWidth=250){
    $l=Label $parent $label $labelX $y $labelWidth
    $c=[Windows.Forms.ComboBox]::new();$c.DropDownStyle='DropDownList';$c.SetBounds($x,$y,$width,29)
    $c.Items.AddRange($items);$c.SelectedIndex=[Math]::Max(0,$selected);$parent.Controls.Add($c);$script:rowLabels[$c]=$l;return $c
}
function Check($parent,[string]$label,[int]$y,[bool]$value){$c=[Windows.Forms.CheckBox]::new();$c.Text=$label;$c.Checked=$value;$c.SetBounds(28,$y,740,28);$parent.Controls.Add($c);return $c}
function Number($parent,[string]$label,[int]$y,[decimal]$value,[decimal]$min=0,[decimal]$max=100,[int]$x=$fieldX,[int]$labelX=32,[int]$labelWidth=250){
    $l=Label $parent $label $labelX $y $labelWidth
    $c=[Windows.Forms.NumericUpDown]::new();$c.Minimum=$min;$c.Maximum=$max;$c.Value=[Math]::Min($max,[Math]::Max($min,$value));$c.SetBounds($x,$y,110,29);$parent.Controls.Add($c);$script:rowLabels[$c]=$l;return $c
}
function Text($parent,[string]$label,[int]$y,[string]$value,[int]$width=$fieldWidth){$l=Label $parent $label 32 $y 250;$c=[Windows.Forms.TextBox]::new();$c.Text=$value;$c.SetBounds($fieldX,$y,$width,29);$parent.Controls.Add($c);$script:rowLabels[$c]=$l;return $c}
function Button($parent,[string]$text,[int]$x,[int]$y,[int]$width=160){$c=[Windows.Forms.Button]::new();$c.Text=$text;$c.SetBounds($x,$y,$width,32);$parent.Controls.Add($c);return $c}
function Message([string]$text){$null=[Windows.Forms.MessageBox]::Show($form,$text,'FM4 Enhanced')}
# Greys out options together with their row label and one-line explanation.
function Set-Option([object[]]$controls,[bool]$available){
    $all=@();foreach($c in $controls){$all+=$c;if($script:rowLabels.ContainsKey($c)){$all+=$script:rowLabels[$c]};if($script:rowHints.ContainsKey($c)){$all+=$script:rowHints[$c]}}
    Set-Fm4Available $all $available
}
function Format-Resolution([int]$scale){return ('{0} × {1}' -f (1280*$scale),(720*$scale))}

$graphics=New-Tab 'Graphics'
$profiles=@('Native','1440p','4K');$upscalers=@('Bilinear','DLSS','FSR','CAS','FSR1','SSAA')
$null=Add-Fm4Section $graphics 'Resolution' 6
$profile=Choice $graphics 'Output resolution' 42 @('1280 × 720 (original)','2560 × 1440','3840 × 2160') ([Array]::IndexOf($profiles,$settings.profile))
$null=Hint $graphics $profile '' 74
$gpu=Text $graphics 'Graphics card' 104 ([string]$settings.gpu_adapter_name)
$null=Hint $graphics $gpu 'Leave empty for automatic selection, or enter part of the card name.' 136

$null=Add-Fm4Section $graphics 'Rendering' 174
# Same settings keys as before: "upscaler" holds the mode, "internal_scale" the render size
# (0 = automatic). Bilinear is only the 1:1 presentation filter of native rendering.
$upscaler=Choice $graphics 'Rendering' 210 @('Native','DLSS','FSR','Native + CAS','Native + FSR 1','Supersampling') $(
    # Supersampling is saved as Bilinear drawing one profile size above the output.
    $savedOutput=[Array]::IndexOf($profiles,$settings.profile)+1
    if($settings.upscaler -eq 'Bilinear' -and $savedOutput -gt 0 -and $savedOutput -lt 3 -and [int]$settings.internal_scale -eq $savedOutput+1 -and -not [bool]$settings.neural_rendering){5}else{[Array]::IndexOf($upscalers,$settings.upscaler)})
$modeHint=Hint $graphics $upscaler '' 242
$savedScale=[int]$settings.internal_scale
$ignoredNativeScale=if($settings.upscaler -notin @('DLSS','FSR') -and -not [bool]$settings.neural_rendering -and $savedScale -ne 0 -and $upscaler.SelectedIndex -ne 5){$savedScale}else{0}
if($savedScale -lt 0 -or $savedScale -gt 3){$savedScale=0}
$scale=Choice $graphics 'Render resolution' 272 @('Automatic','1280 × 720','2560 × 1440','3840 × 2160') $savedScale
$scaleHint=Hint $graphics $scale '' 304
$renderSummary=Label $graphics '' 32 330 758
$renderSummary.Font=[Fm4Ui.Palette]::Make(11.5,$true)
$frameRates=@(60,120,144,165,240)
$frameRateValue=if($settings.ContainsKey('frame_rate_limit')){[int]$settings.frame_rate_limit}else{60}
$frameRate=Choice $graphics 'Frame rate limit' 354 @('60','120','144','165','240') ([Array]::IndexOf($frameRates,$frameRateValue))
$null=Hint $graphics $frameRate 'Above 60 is experimental.' 386

$null=Add-Fm4Section $graphics 'DLSS 5 Neural Rendering' 414
$nr=Check $graphics 'Enable Neural Rendering (experimental)' 450 ([bool]$settings.neural_rendering)
$null=Hint $graphics $nr 'Relights the image on NVIDIA RTX cards.' 478 52
$neuralPaths=@('auto','official','community')
$neuralPathValue=if($settings.ContainsKey('neural_path') -and $settings.neural_path){[string]$settings.neural_path}else{'auto'}
$neuralPath=Choice $graphics 'Compatibility' 508 @('Automatic','NVIDIA (RTX 50)','Community (RTX 40)') ([Array]::IndexOf($neuralPaths,$neuralPathValue))
$pathStatus=Hint $graphics $null '' 540 32
$pathWarning=Hint $graphics $null '' 562 32
$passes=Number $graphics 'Passes' 594 ([int]$settings.neural_passes) 1 3
$strength=Number $graphics 'Strength (%)' 594 ([decimal]$settings.neural_strength*100) 0 100 650 430 200
$preset=Choice $graphics 'Model preset' 632 @('Automatic','1','2','3') ([int]$settings.neural_preset) $fieldX 110
$style=Choice $graphics 'Style' 632 @('Standard','Natural','Cinematic') ([int]$settings.neural_style) 650 110 430 200
$null=Hint $graphics $passes 'Extra passes take more GPU time.' 664
$nrRuntimeValue=if($settings.neural_runtime_folder){[string]$settings.neural_runtime_folder}else{'.local/neural-runtime'}
$nrRuntimeFolder=Text $graphics 'Runtime folder' 694 $nrRuntimeValue 330
$nrRuntimeBrowse=Button $graphics 'Choose folder…' 630 693 130
$nrRuntimeBrowse.Add_Click({$dialog=[Windows.Forms.FolderBrowserDialog]::new();try{if($dialog.ShowDialog($form) -eq 'OK'){$nrRuntimeFolder.Text=$dialog.SelectedPath}}finally{$dialog.Dispose()}})
$nrStatus=Hint $graphics $nrRuntimeFolder '' 726
$null=Hint $graphics $null 'Requires your own nvngx_dlssnr.dll and nvngx_dlss.dll files.' 748
$null=Label $graphics '' 32 768 10

$textures=New-Tab 'Textures'
$null=Add-Fm4Section $textures 'Texture pack' 6
# texture_mode: auto uses the pack only once its manifest is marked finished (tools/Texture-Packs.py
# finish); older settings files map enable_textures to on/off.
$textureModes=@('auto','on','off')
$savedTextureMode=if($current.ContainsKey('texture_mode')){[string]$current.texture_mode}elseif($current.ContainsKey('enable_textures')){if($current.enable_textures){'on'}else{'off'}}elseif($settings.ContainsKey('texture_mode')){[string]$settings.texture_mode}else{'auto'}
$textureMode=Choice $textures 'Texture pack' 42 @('Automatic (finished packs only)','On','Off') ([Math]::Max(0,[Array]::IndexOf($textureModes,$savedTextureMode)))
$null=Hint $textures $textureMode 'Textures missing from the pack use the originals.' 74
$texturePath=Text $textures 'Pack folder' 102 $(if($settings.texture_pack){[string]$settings.texture_pack}else{'textures/Enhanced'})
$browse=Button $textures 'Choose folder…' $fieldX 140 180
$import=Button $textures 'Import ZIP…' ($fieldX+196) 140 180
$null=Hint $textures $texturePath 'ZIP packs are extracted to textures/.' 178
$textureStatus=Hint $textures $null '' 200
$null=Add-Fm4Section $textures 'Options' 244
$rgba=Check $textures 'Load uncompressed textures' 280 ([bool]$settings.enable_rgba_textures)
$null=Hint $textures $rgba 'Leave off unless your pack needs uncompressed RGBA textures.' 308 52
$null=Add-Fm4Section $textures 'Filtering' 352
# texture_mip_bias runtime override: 0 sharp, 0.5 balanced (default), 1 the original 720p mip choice.
$mipBiases=@(0.0,0.5,1.0)
$savedMipBias=if($settings.runtime_overrides -and $settings.runtime_overrides.ContainsKey('texture_mip_bias')){[double]$settings.runtime_overrides.texture_mip_bias}else{0.5}
$mipBias=Choice $textures 'Distant texture detail' 388 @('Sharp (may shimmer)','Balanced','Soft (original detail)') ([Array]::IndexOf($mipBiases,$savedMipBias))
$null=Hint $textures $mipBias 'Softer settings reduce shimmer on roads and grass.' 420
$null=Label $textures '' 32 448 10
# Downloadable content (added 2026-10-23): dlc_folder holds extracted FM4 content packages, for
# example the Project Forza Plus 4 "Disc 2 and DLCs" folder. Run-Enhanced links each package into
# the profile and writes the content header FM4 needs to mount it.
$content=New-Tab 'Content'
$null=Add-Fm4Section $content 'Downloadable content' 6
$dlcPath=Text $content 'DLC folder' 42 $(if($settings.dlc_folder){[string]$settings.dlc_folder}else{''})
$dlcBrowse=Button $content 'Choose folder…' $fieldX 80 180
$null=Hint $content $dlcPath 'Choose extracted FM4 DLC, or leave empty for Disc 2 content only.' 118 52
$dlcBrowse.Add_Click({$dialog=[Windows.Forms.FolderBrowserDialog]::new();try{if($dialog.ShowDialog($form) -eq 'OK'){$dlcPath.Text=$dialog.SelectedPath}}finally{$dialog.Dispose()}})
function Get-TexturePackStatus([string]$folder) {
    if(-not $folder){return 'No pack folder selected.'}
    if(-not [IO.Path]::IsPathRooted($folder)){$folder=Join-Path $root $folder}
    if((Split-Path -Leaf $folder) -eq 'replacements'){$folder=Split-Path -Parent $folder}
    if(-not(Test-Path -LiteralPath $folder -PathType Container)){return 'Pack not installed; using original textures.'}
    $manifest=Join-Path $folder 'manifest.json'
    if(-not(Test-Path -LiteralPath $manifest -PathType Leaf)){return 'Pack has no manifest; select On to use it.'}
    try{$data=Get-Content -LiteralPath $manifest -Raw -Encoding UTF8|ConvertFrom-Json}catch{return 'Cannot read the pack manifest.'}
    $count=@($data.PSObject.Properties['textures']|ForEach-Object{$_.Value}).Count
    $complete=$data.PSObject.Properties['complete']
    if($null -ne $complete -and $complete.Value -eq $true){return "Pack ready: $count textures."}
    return "Pack unfinished: $count textures; Automatic uses originals."
}
function Update-TextureStatus{$textureStatus.Text=Get-TexturePackStatus $texturePath.Text.Trim()}
$texturePath.Add_TextChanged({Update-TextureStatus})
Update-TextureStatus
$browse.Add_Click({$dialog=[Windows.Forms.FolderBrowserDialog]::new();try{if($dialog.ShowDialog($form) -eq 'OK'){$texturePath.Text=$dialog.SelectedPath;$textureMode.SelectedIndex=1}}finally{$dialog.Dispose()}})
$import.Add_Click({
    $dialog=[Windows.Forms.OpenFileDialog]::new();$dialog.Filter='Texture ZIP (*.zip)|*.zip'
    try{
        if($dialog.ShowDialog($form) -ne 'OK'){return}
        $name='Imported-'+(Get-Date -Format 'yyyyMMdd-HHmmss')
        $python=(Get-Command python -ErrorAction Stop).Source
        $info=[Diagnostics.ProcessStartInfo]::new($python);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        Set-Fm4ProcessArguments -StartInfo $info -ArgumentList @((Join-Path $PSScriptRoot 'Texture-Packs.py'),'import',$dialog.FileName,'--name',$name,'--textures-root',(Join-Path $root 'textures'))
        $process=[Diagnostics.Process]::Start($info);$output=$process.StandardOutput.ReadToEnd();$errorText=$process.StandardError.ReadToEnd();$process.WaitForExit()
        try{if($process.ExitCode -ne 0){throw $errorText};$texturePath.Text=Join-Path $root "textures/$name";$textureMode.SelectedIndex=1;Message 'Texture pack imported.'}finally{$process.Dispose()}
    }catch{Message $_.Exception.Message}finally{$dialog.Dispose()}
})

$controls=New-Tab 'Force feedback'
$null=Add-Fm4Section $controls 'Wheel' 6
$wheelEnabled=Check $controls 'Enable wheel and pedals' 42 ((Get-Ini wheel enabled 1) -eq '1')
$null=Hint $controls $wheelEnabled 'Set bindings on the Controls page.' 70 52
$rotation=Number $controls 'Wheel rotation (degrees)' 98 ([decimal]::Parse((Get-Ini wheel rotation_degrees 900),[Globalization.CultureInfo]::InvariantCulture)) 90 3600
$rotation.Increment=90
$null=Hint $controls $rotation 'Match the rotation set in your wheel driver.' 130
$fullRotation=Check $controls 'Match in-game wheel rotation' 156 ((Get-Ini wheel full_rotation 1) -eq '1')
$null=Hint $controls $fullRotation 'Off limits the in-game wheel animation to 90 degrees.' 184 52
$null=Add-Fm4Section $controls 'Force feedback' 222
$ffb=Check $controls 'Enable force feedback' 258 ((Get-Ini force enabled 1) -eq '1')
$null=Hint $controls $ffb '' 286 52
$gain=Number $controls 'Overall strength (%)' 314 ([decimal]::Parse((Get-Ini force gain .4),[Globalization.CultureInfo]::InvariantCulture)*100)
$detail=Number $controls 'Vibration detail (%)' 352 ([decimal]::Parse((Get-Ini force detail .5),[Globalization.CultureInfo]::InvariantCulture)*100)
$null=Hint $controls $detail 'Adds kerb, road, engine and impact vibrations.' 384
$road=Number $controls 'Road texture (%)' 412 ([decimal]::Parse((Get-Ini force road .3),[Globalization.CultureInfo]::InvariantCulture)*100)
$null=Hint $controls $road 'Adds small steering kicks from bumps under the front tyres.' 444
$weight=Number $controls 'Steering weight (%)' 472 ([decimal]::Parse((Get-Ini force steering_weight 3.5),[Globalization.CultureInfo]::InvariantCulture)*100) 100 500
$null=Hint $controls $weight '100% matches the original steering forces.' 504
$centering=Number $controls 'Centring spring (%)' 532 ([decimal]::Parse((Get-Ini force centering 0),[Globalization.CultureInfo]::InvariantCulture)*100)
$null=Hint $controls $centering 'Adds a pull towards the centre.' 564
$damping=Number $controls 'Damping (%)' 592 ([decimal]::Parse((Get-Ini force damping .25),[Globalization.CultureInfo]::InvariantCulture)*100) 0 200
$null=Hint $controls $damping 'Adds resistance to wheel movement.' 624
$modes=@('game','telemetry');$forceMode=Choice $controls 'Force source' 652 @('Original game','Car physics') ([Array]::IndexOf($modes,(Get-Ini force mode telemetry)))
$null=Hint $controls $forceMode 'Car physics gives smoother steering forces.' 684
$backends=@('auto','native','software');$backend=Choice $controls 'Effect type' 712 @('Automatic','Wheel driver','Software + wheel driver') ([Array]::IndexOf($backends,(Get-Ini force backend auto)))
$null=Hint $controls $backend 'Try another mode if force feedback feels wrong or is missing.' 744
$invert=Check $controls 'Invert force direction' 772 ((Get-Ini force invert 0) -eq '1')
$null=Add-Fm4Section $controls 'Controller' 816
$adaptive=Check $controls 'DualSense adaptive triggers' 852 ((Get-Ini controller adaptive_triggers 1) -eq '1')
$null=Hint $controls $adaptive 'Brake and throttle resistance on a DualSense controller.' 880 52
$adaptiveGain=Number $controls 'Trigger strength (%)' 908 ([decimal]::Parse((Get-Ini controller adaptive_gain .35),[Globalization.CultureInfo]::InvariantCulture)*100)
$rumble=Number $controls 'Controller vibration (%)' 946 ([decimal]::Parse((Get-Ini controller rumble_gain 1),[Globalization.CultureInfo]::InvariantCulture)*100)
$null=Hint $controls $rumble 'Use the controller for menus.' 978
$null=Label $controls '' 32 1006 10
$controlsUi=New-Fm4ControlsPage -Pages $tabs -EditionRoot $root -WheelText $wheelText -WheelPath $wheelPath -NoLiveInput:$SelfTest
$controlsUi.RawButton.Add_Click({if(-not(Test-Path -LiteralPath $wheelPath)){New-Item -ItemType Directory (Split-Path $wheelPath) -Force|Out-Null;Write-Fm4Utf8File -Path $wheelPath -Text $wheelText};Start-Process notepad.exe -ArgumentList ('"'+$wheelPath+'"')})

$advanced=New-Tab 'Advanced'
$null=Add-Fm4Section $advanced 'Performance' 6
$batch=Check $advanced 'Batch graphics copies' 42 $(if($settings.ContainsKey('readback_batch')){[bool]$settings.readback_batch}else{$true})
$null=Hint $advanced $batch 'Groups graphics copies to improve race performance.' 70 52
$gpuCompletion=Check $advanced 'Finish graphics copies on GPU' 96 $(if($settings.runtime_overrides.ContainsKey('d3d12_readback_resolve_gpu_completion')){[bool]$settings.runtime_overrides.d3d12_readback_resolve_gpu_completion}else{$true})
$null=Hint $advanced $gpuCompletion 'Reduces CPU waiting when graphics copies are batched.' 124 52
$bulkRegisters=Check $advanced 'Optimise CPU command processing' 150 $(if($settings.runtime_overrides.ContainsKey('gpu_bulk_type0_register_writes')){[bool]$settings.runtime_overrides.gpu_bulk_type0_register_writes}else{$true})
$null=Hint $advanced $bulkRegisters 'Reduces CPU work without changing the image.' 178 52
$null=Add-Fm4Section $advanced 'Visibility' 216
$occlusionSamples=Number $advanced 'Visibility fallback value' 252 $(if($settings.runtime_overrides.ContainsKey('query_occlusion_fake_sample_count')){[decimal]$settings.runtime_overrides.query_occlusion_fake_sample_count}else{1000}) 0 ([int]::MaxValue)
$occlusionSamples.Width=160;$occlusionSamples.Increment=100;$occlusionSamples.ThousandsSeparator=$true
$null=Hint $advanced $occlusionSamples 'Low values can hide roads and scenery when visibility checks fail.' 284
$cumulativeVisibility=Check $advanced 'Combine visibility results' 310 $(if($settings.runtime_overrides.ContainsKey('gpu_occlusion_query_cumulative_fallback')){[bool]$settings.runtime_overrides.gpu_occlusion_query_cumulative_fallback}else{$true})
$null=Hint $advanced $cumulativeVisibility 'Combines visibility checks within each frame.' 338 52
$null=Add-Fm4Section $advanced 'Testing' 376
$null=Hint $advanced $null 'Leave these off for normal play.' 410 32
$center=Check $advanced 'Sample scaled readbacks at pixel centres' 436 ([bool]$settings.readback_center_sample)
$null=Hint $advanced $center 'Compares pixel-centre and corner sampling.' 464 52
$hostTargets=Check $advanced 'Experimental host render targets' 488 ([bool]$settings.host_render_targets)
$null=Hint $advanced $hostTargets 'Tests an alternative drawing method that may cause visual errors.' 516 52
$readback=Check $advanced 'Use readback heap instead of host copies' 540 ([bool]$settings.readback_heap)
$null=Hint $advanced $readback 'Tests an alternative method for reading graphics copies.' 568 52
$completionBoundary=Check $advanced 'Submit each GPU completion separately' 592 $(if($settings.runtime_overrides.ContainsKey('d3d12_readback_resolve_completion_boundary')){[bool]$settings.runtime_overrides.d3d12_readback_resolve_completion_boundary}else{$false})
$null=Hint $advanced $completionBoundary 'Tests GPU ordering with separate completion submissions.' 620 52
$primarySubmit=Check $advanced 'Submit at every command-buffer end' 644 $(if($settings.runtime_overrides.ContainsKey('d3d12_submit_on_primary_buffer_end')){[bool]$settings.runtime_overrides.d3d12_submit_on_primary_buffer_end}else{$false})
$null=Hint $advanced $primarySubmit 'Tests GPU ordering at command-buffer boundaries.' 672 52
$raw=Button $advanced 'Edit graphics settings…' 32 712 240
$docs=Button $advanced 'Open configuration guide' 290 712 240
$null=Label $advanced '' 32 750 10
$raw.Add_Click({if(Test-Path -LiteralPath $settingsPath){Start-Process notepad.exe -ArgumentList ('"'+$settingsPath+'"')}else{Message 'Save settings first.'}})
$docs.Add_Click({Start-Process notepad.exe -ArgumentList ('"'+(Join-Path $root 'docs/ENHANCEMENTS.md')+'"')})
$pfpUi=New-Pfp4SettingsTab -TabControl $tabs -SettingsRoot $settings -EditionRoot $root

# Live state: explanations follow the choices and options that do not apply are greyed out.
$displayAdapters=@(Get-Fm4DisplayAdapters)
$updatingGraphics=$false
function Get-NeuralPlan{return Resolve-Fm4NeuralPath -Requested $neuralPaths[[Math]::Max(0,$neuralPath.SelectedIndex)] -AdapterName $gpu.Text.Trim() -Adapters $script:displayAdapters}
function Resolve-RuntimeFolder([string]$folder){$folder=$folder.Trim();if(-not $folder){return ''};if(-not [IO.Path]::IsPathRooted($folder)){$folder=Join-Path $root $folder};return [IO.Path]::GetFullPath($folder)}
function Get-RenderPlan{
    $mode=$upscalers[[Math]::Max(0,$upscaler.SelectedIndex)];$outputScale=[Math]::Max(0,$profile.SelectedIndex)+1
    $temporal=$mode -in @('DLSS','FSR');$usesScale=$temporal -or $nr.Checked
    $internal=if($mode -eq 'SSAA'){$outputScale+1}elseif($usesScale){$script:savedScale}else{0}
    # Mirrors Run-Enhanced.ps1: DLSS/FSR default to 720p input; native modes draw at the output size.
    $renderScale=if($temporal){1}else{$outputScale};if($internal){$renderScale=$internal}
    return @{Mode=$mode;Output=$outputScale;Temporal=$temporal;UsesScale=$usesScale;Internal=$internal;Render=$renderScale}
}
function Update-GraphicsPage{
    if($script:updatingGraphics){return}
    $script:updatingGraphics=$true
    try{
        $plan=Get-RenderPlan
        $modeHint.Text=switch($plan.Mode){
            'Bilinear'{'Renders at the output resolution.'}
            'DLSS'{'Upscales a lower resolution image on NVIDIA RTX cards.'}
            'FSR'{'Upscales a lower resolution image on most graphics cards.'}
            'CAS'{'Sharpens the native image.'}
            'FSR1'{'Applies the FSR 1 filter to the native image.'}
            'SSAA'{'Renders at a higher resolution for smoother edges, with a performance cost.'}
        }
        Set-Option @($scale) $plan.UsesScale
        if($plan.UsesScale){
            $scale.Items[0]=if($plan.Temporal){'Automatic (1280 × 720)'}else{'Automatic ('+(Format-Resolution $plan.Output)+')'}
            $scale.SelectedIndex=$script:savedScale
            $scaleHint.Text=if($plan.Temporal){'Higher render resolutions improve detail but take more GPU time.'}else{'Automatic uses the output resolution.'}
        }else{
            # Native without Neural Rendering always draws at the output size.
            $scale.Items[0]='Automatic';$scale.SelectedIndex=$plan.Output
            $scaleHint.Text='Native modes use the output resolution.'
        }
        $tooLarge=($plan.UsesScale -and $plan.Render -gt $plan.Output) -or ($plan.Mode -eq 'SSAA' -and $plan.Output -ge 3)
        $text=if($tooLarge){'Render resolution must not be larger than the output resolution.'}
              elseif($plan.Mode -eq 'SSAA' -and $plan.Output -ge 3){'Supersampling is unavailable at 3840 × 2160.'}
              elseif($plan.Mode -eq 'SSAA'){'Supersampling: '+(Format-Resolution $plan.Render)+' to '+(Format-Resolution $plan.Output)+'.'}
              elseif($plan.Temporal){'Render '+(Format-Resolution $plan.Render)+'; '+$(if($plan.Mode -eq 'DLSS'){'DLSS'}else{'FSR'})+' output '+(Format-Resolution $plan.Output)+'.'}
              elseif($plan.Render -eq $plan.Output){'Native: '+(Format-Resolution $plan.Output)+'.'}
              else{'Render '+(Format-Resolution $plan.Render)+'; stretched to '+(Format-Resolution $plan.Output)+'.'}
        if($nr.Checked -and -not $tooLarge){$text+=' Neural Rendering: '+(Format-Resolution $plan.Render)+'.'}
        if($script:ignoredNativeScale -and -not $plan.UsesScale){$text+=' Saved render scale ignored.'}
        $renderSummary.Text=$text;$renderSummary.ForeColor=if($tooLarge){[Fm4Ui.Palette]::Accent}else{[Fm4Ui.Palette]::Text}
        Set-Option @($neuralPath,$passes,$strength,$preset,$style,$nrRuntimeFolder,$nrRuntimeBrowse) $nr.Checked
        $neural=Get-NeuralPlan
        $pathStatus.Text=$neural.Summary -replace ' \((?:un)?tested[^)]*\)$',''
        $pathWarning.Text=if($neural.Warnings.Count){($neural.Warnings -join ' ') -replace ' is untested here; it needs a community runtime built for this GPU and costs much more GPU time\.',' needs a compatible community runtime and may run slowly.' -replace 'The GPU could not be detected; assuming the community path\.','GPU not detected; using community compatibility.' -replace 'Neural Rendering needs an NVIDIA RTX GPU; detected: [^.]+\.','Neural Rendering needs an NVIDIA RTX GPU.' -replace 'The official path is meant for RTX 50\. On other GPUs the driver is expected to decline; the direct bridge is then used if it is built\.','NVIDIA compatibility needs RTX 50; other cards may use the fallback.' -replace 'NVIDIA lists driver (\S+) or newer for DLSS 5; this GPU reports (\S+)\.','DLSS 5 needs driver $1 or newer; installed: $2.'}else{''}
        $pathWarning.ForeColor=if($nr.Checked -and $neural.Warnings.Count){[Fm4Ui.Palette]::Accent}else{[Fm4Ui.Palette]::Dim}
        $folder=Resolve-RuntimeFolder $nrRuntimeFolder.Text
        $missing=@(foreach($file in @('nvngx_dlssnr.dll','nvngx_dlss.dll')){if(-not $folder -or -not(Test-Path -LiteralPath (Join-Path $folder $file))){$file}})
        $bridge=Test-Path -LiteralPath (Join-Path $root '.local/graphics-build/nvngx.dll_fm4.dll')
        $files=if($missing.Count){'Missing: '+($missing -join ', ')+'.'}else{'Runtime files found.'}
        $bridgeText=switch($neural.Path){
            'community'{if($bridge){' Compatibility support ready.'}else{' Compatibility support missing; see docs/NEURAL_RENDERING.md.'}}
            'official'{if($bridge){' Fallback ready.'}else{' Fallback unavailable.'}}
            default{''}
        }
        $nrStatus.Text=$files+$bridgeText
    }finally{$script:updatingGraphics=$false}
}
$scale.Add_SelectedIndexChanged({if(-not $script:updatingGraphics -and $scale.Enabled){$script:savedScale=$scale.SelectedIndex};Update-GraphicsPage})
foreach($control in @($profile,$upscaler,$neuralPath)){$control.Add_SelectedIndexChanged({Update-GraphicsPage})}
$nr.Add_CheckedChanged({Update-GraphicsPage})
foreach($control in @($gpu,$nrRuntimeFolder)){$control.Add_TextChanged({Update-GraphicsPage})}
function Update-OptionPages{
    Set-Option @($gain,$detail,$road,$weight,$centering,$damping,$forceMode,$backend,$invert) ($wheelEnabled.Checked -and $ffb.Checked)
    Set-Option @($ffb,$rotation,$fullRotation) $wheelEnabled.Checked
    Set-Option @($adaptiveGain) $adaptive.Checked
    Set-Option @($texturePath,$browse,$rgba) ($textureMode.SelectedIndex -ne 2)
    Set-Option @($gpuCompletion,$completionBoundary) $batch.Checked
}
foreach($control in @($wheelEnabled,$ffb,$adaptive,$batch)){$control.Add_CheckedChanged({Update-OptionPages})}
$textureMode.Add_SelectedIndexChanged({Update-OptionPages})

function Read-Controls($baseSettings=$script:settings) {
    if($profile.SelectedIndex -lt 0 -or $upscaler.SelectedIndex -lt 0 -or $scale.SelectedIndex -lt 0 -or $neuralPath.SelectedIndex -lt 0){throw 'Choose output, rendering and render resolution settings.'}
    $runtimeFolder=Resolve-RuntimeFolder $nrRuntimeFolder.Text
    if($nr.Checked -and -not $SelfTest){
        $neural=Get-NeuralPlan
        if(-not $neural.Supported){throw ($neural.Warnings -join ' ')}
        if(-not $runtimeFolder -or -not(Test-Path -LiteralPath (Join-Path $runtimeFolder 'nvngx_dlssnr.dll')) -or -not(Test-Path -LiteralPath (Join-Path $runtimeFolder 'nvngx_dlss.dll'))){throw 'Choose a folder with matching nvngx_dlssnr.dll and nvngx_dlss.dll files.'}
        if($neural.Path -eq 'community' -and -not(Test-Path -LiteralPath (Join-Path $root '.local/graphics-build/nvngx.dll_fm4.dll'))){throw 'Neural Rendering support is missing; see docs/NEURAL_RENDERING.md.'}
    }
    $render=Get-RenderPlan
    $s=$baseSettings.Clone()
    $s.forza_plus=Read-Pfp4Controls -Controls $pfpUi -BaseSettings $baseSettings -EditionRoot $root
    # Native modes without Neural Rendering always save internal_scale 0 (true native).
    $s.profile=$profiles[$profile.SelectedIndex];$s.upscaler=$upscalers[$upscaler.SelectedIndex];$s.internal_scale=[int]$render.Internal
    if($frameRate.SelectedIndex -lt 0){throw 'Choose a frame rate limit.'};$s.frame_rate_limit=[int]$frameRates[$frameRate.SelectedIndex]
    if($s.upscaler -eq 'SSAA'){if($render.Output -ge 3){throw 'Supersampling is unavailable at 3840 × 2160.'};$s.upscaler='Bilinear'}
    $s.gpu_adapter_name=$gpu.Text.Trim();$s.neural_rendering=$nr.Checked;$s.neural_passes=[int]$passes.Value;$s.neural_preset=$preset.SelectedIndex;$s.neural_style=$style.SelectedIndex;$s.neural_strength=[double]$strength.Value/100;$s.neural_runtime_folder=$nrRuntimeFolder.Text.Trim()
    $s.neural_path=$neuralPaths[$neuralPath.SelectedIndex]
    $s.texture_mode=$textureModes[[Math]::Max(0,$textureMode.SelectedIndex)];if($s.ContainsKey('enable_textures')){$s.Remove('enable_textures')}
    $s.texture_pack=$texturePath.Text.Trim();$s.enable_rgba_textures=$rgba.Checked
    if($dlcPath.Text.Trim()){$s.dlc_folder=$dlcPath.Text.Trim()}elseif($s.ContainsKey('dlc_folder')){$s.Remove('dlc_folder')}
    $s.readback_batch=$batch.Checked;$s.readback_center_sample=$center.Checked;$s.host_render_targets=$hostTargets.Checked;$s.readback_heap=$readback.Checked
    $s.runtime_overrides=if($s.runtime_overrides){$s.runtime_overrides.Clone()}else{@{}}
    $s.runtime_overrides.query_occlusion_fake_sample_count=[int]$occlusionSamples.Value
    $s.runtime_overrides.texture_mip_bias=[double]$mipBiases[[Math]::Max(0,$mipBias.SelectedIndex)]
    $s.runtime_overrides.gpu_bulk_type0_register_writes=$bulkRegisters.Checked
    $s.runtime_overrides.d3d12_readback_resolve_gpu_completion=$gpuCompletion.Checked
    $s.runtime_overrides.gpu_occlusion_query_cumulative_fallback=$cumulativeVisibility.Checked
    $s.runtime_overrides.d3d12_readback_resolve_completion_boundary=$completionBoundary.Checked
    $s.runtime_overrides.d3d12_submit_on_primary_buffer_end=$primarySubmit.Checked
    $outputScale=$profile.SelectedIndex+1
    if(($nr.Checked -or $s.upscaler -in @('DLSS','FSR')) -and $s.internal_scale -gt $outputScale){throw 'Render resolution must not be larger than the output resolution.'}
    if($s.texture_mode -eq 'on' -and $s.texture_pack){
        $folder=$s.texture_pack;if(-not [IO.Path]::IsPathRooted($folder)){$folder=Join-Path $root $folder}
        if(-not(Test-Path -LiteralPath $folder -PathType Container)){throw 'Texture pack folder not found.'}
    }
    return $s
}
function Read-WheelControls {
    $text=if(-not $SelfTest -and (Test-Path -LiteralPath $wheelPath)){Get-Content -LiteralPath $wheelPath -Raw -Encoding UTF8}else{$script:wheelText}
    $values=@(@('wheel','enabled',[int]$wheelEnabled.Checked),@('force','enabled',[int]$ffb.Checked),@('force','gain',([double]$gain.Value/100).ToString([Globalization.CultureInfo]::InvariantCulture)),@('force','detail',([double]$detail.Value/100).ToString([Globalization.CultureInfo]::InvariantCulture)),@('force','invert',[int]$invert.Checked),@('force','mode',$modes[[Math]::Max(0,$forceMode.SelectedIndex)]),@('force','road',([double]$road.Value/100).ToString([Globalization.CultureInfo]::InvariantCulture)),@('force','steering_weight',([double]$weight.Value/100).ToString([Globalization.CultureInfo]::InvariantCulture)),@('force','centering',([double]$centering.Value/100).ToString([Globalization.CultureInfo]::InvariantCulture)),@('force','damping',([double]$damping.Value/100).ToString([Globalization.CultureInfo]::InvariantCulture)),@('wheel','rotation_degrees',[string][int]$rotation.Value),@('wheel','full_rotation',[int]$fullRotation.Checked),@('force','backend',$backends[$backend.SelectedIndex]),@('controller','adaptive_triggers',[int]$adaptive.Checked),@('controller','adaptive_gain',([double]$adaptiveGain.Value/100).ToString([Globalization.CultureInfo]::InvariantCulture)),@('controller','rumble_gain',([double]$rumble.Value/100).ToString([Globalization.CultureInfo]::InvariantCulture)))
    foreach($entry in $values){$text=Set-Ini $text $entry[0] $entry[1] ([string]$entry[2])}
    return (Write-Fm4BindingsToIni $controlsUi $text)
}
function Commit-SettingsPair([string]$settingsText,[string]$wheelText) {
    $settingsTemporary=$settingsPath+'.tmp.'+[guid]::NewGuid().ToString('N')
    $wheelTemporary=$wheelPath+'.tmp.'+[guid]::NewGuid().ToString('N')
    New-Item -ItemType Directory -Path (Split-Path $settingsPath),(Split-Path $wheelPath) -Force|Out-Null
    try {
        Write-Fm4Utf8File -Path $settingsTemporary -Text $settingsText
        Write-Fm4Utf8File -Path $wheelTemporary -Text $wheelText
        $settingsExisted=Test-Path -LiteralPath $settingsPath -PathType Leaf
        $wheelExisted=Test-Path -LiteralPath $wheelPath -PathType Leaf
        $settingsBytes=if($settingsExisted){[IO.File]::ReadAllBytes($settingsPath)}else{$null}
        $wheelBytes=if($wheelExisted){[IO.File]::ReadAllBytes($wheelPath)}else{$null}
        try {
            Move-Item -LiteralPath $settingsTemporary -Destination $settingsPath -Force
            if($script:injectSecondCommitFailure){throw 'Injected second settings commit failure.'}
            Move-Item -LiteralPath $wheelTemporary -Destination $wheelPath -Force
        } catch {
            $commitError=$_.Exception.Message
            $rollbackErrors=@()
            foreach($target in @(
                @{Path=$settingsPath;Existed=$settingsExisted;Bytes=$settingsBytes},
                @{Path=$wheelPath;Existed=$wheelExisted;Bytes=$wheelBytes}
            )){
                try {
                    if($target.Existed){[IO.File]::WriteAllBytes($target.Path,$target.Bytes)}
                    elseif(Test-Path -LiteralPath $target.Path){Remove-Item -LiteralPath $target.Path -Force}
                } catch {$rollbackErrors+=($_.Exception.Message)}
            }
            if($rollbackErrors.Count){throw "Could not save settings ($commitError); restore failed: $($rollbackErrors -join '; ')"}
            throw "Could not save settings; previous settings were restored. $commitError"
        }
    } finally {
        foreach($temporary in @($settingsTemporary,$wheelTemporary)){
            if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue}
        }
    }
}
function Save-Controls {
    $currentSettings=$defaults.Clone()
    if(Test-Path -LiteralPath $settingsPath){
        $current=Get-Content -LiteralPath $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Fm4Json
        foreach($key in $current.Keys){$currentSettings[$key]=$current[$key]}
    }
    $s=Read-Controls $currentSettings;$wheel=Read-WheelControls
    $settingsText=$s|ConvertTo-Json -Depth 12
    Commit-SettingsPair $settingsText $wheel
    $script:settings=$s;$script:wheelText=$wheel
}
$play=Add-Fm4Prompt $shell 'A' 'Save and play' 220
$save=Add-Fm4Prompt $shell 'X' 'Save' 140
$close=Add-Fm4Prompt $shell 'B' 'Close' 140
Complete-Fm4Shell $shell
Update-GraphicsPage;Update-OptionPages
Register-Fm4ControlsPage $controlsUi $form $tabs
$save.Add_Click({try{Save-Controls;Message 'Settings saved.'}catch{Message $_.Exception.Message}})
$play.Add_Click({try{Save-Controls;$form.DialogResult='OK';$form.Close()}catch{Message $_.Exception.Message}})
$close.Add_Click({$form.Close()})
if($SelfTest){
    try{
        Test-Pfp4ProposalContract
        if($pfpUi.Mode.SelectedIndex -ne 0 -or $pfpUi.Mode.Text -ne 'Normal' -or $pfpUi.Physics.SelectedIndex -ne 0 -or $pfpUi.Physics.Text -ne 'Mod default'){throw "Enhanced PFP4 selectors did not initialize visibly: mode=$($pfpUi.Mode.SelectedIndex)/'$($pfpUi.Mode.Text)' physics=$($pfpUi.Physics.SelectedIndex)/'$($pfpUi.Physics.Text)'"}
        $savedWheelText=$script:wheelText
        try{
            $script:wheelText=$wheelDefaults
            if((Get-Ini wheel enabled 0) -ne '1' -or (Get-Ini force enabled 0) -ne '1' -or (Get-Ini force gain 0) -ne '0.4'){throw 'First-launch wheel/force defaults differ from the runtime.'}
        }finally{$script:wheelText=$savedWheelText}
        if($PreviewDirectory){Save-Fm4Previews $shell $PreviewDirectory}
        $pfpUi.Enabled.Checked=$true;$pfpUi.Mode.SelectedIndex=2;$pfpUi.Physics.SelectedIndex=5;$pfpUi.TargetFps.Value=120.5
        $pfpUi.Camera[0].Inherit.Checked=$false;$pfpUi.Camera[0].Number.Value=71.5
        $uiPfp=Read-Pfp4Controls -Controls $pfpUi -BaseSettings $settings -EditionRoot $root
        $uiJson=$uiPfp|ConvertTo-Json -Depth 12|ConvertFrom-Fm4Json
        if(-not $uiJson.enabled -or $uiJson.mode -ne 'sandbox' -or $uiJson.physics_preset -ne 'custom' -or $uiJson.physics_target_fps -ne 120.5 -or $uiJson.camera_fov.follow_low -ne 71.5 -or $uiJson.camera_fov.driver -ne $null){throw 'PFP4 advanced control values did not survive the JSON round trip.'}
        if($profile.SelectedIndex -lt 0 -or $upscaler.SelectedIndex -lt 0 -or $scale.SelectedIndex -lt 0 -or $preset.SelectedIndex -lt 0 -or $style.SelectedIndex -lt 0 -or $frameRate.SelectedIndex -lt 0){throw 'An initial settings selection is missing.'}
        Write-Output "Initial selections: $($profile.Text); $($upscaler.Text); $($scale.Text); $($preset.Text); $($style.Text)."
        Write-Output "Neural Rendering on this PC: $($pathStatus.Text)"
        function Save-PagePreview($page,[string]$name,[int]$scrollY=0){
            if(-not $PreviewDirectory){return}
            $tabs.SelectedTab=$page;foreach($nav in $shell.NavButtons){$nav.Selected=($nav.Tag -eq $page)}
            # An offscreen form cannot scroll, so shift the page contents to show the lower part.
            $children=@($page.Controls);$above=@($children|Where-Object{$_.Bottom -le $scrollY})
            foreach($child in $above){$page.Controls.Remove($child)};foreach($child in $children){$child.Top-=$scrollY}
            try{Save-Fm4Preview $form (Join-Path $PreviewDirectory $name)}
            finally{foreach($child in $children){$child.Top+=$scrollY};foreach($child in $above){$page.Controls.Add($child)}}
        }
        # Native rendering is labelled as native: the render size follows the output and is saved as automatic.
        $nr.Checked=$false;$upscaler.SelectedIndex=0;$profile.SelectedIndex=2
        if($scale.Enabled -or $scale.Text -ne '3840 × 2160' -or $renderSummary.Text -notmatch 'Native: 3840 × 2160'){throw "Native 4K was not presented as true native rendering: '$($scale.Text)' / '$($renderSummary.Text)'."}
        $native=Read-Controls;if($native.upscaler -ne 'Bilinear' -or $native.internal_scale -ne 0 -or $native.profile -ne '4K'){throw 'Native 4K did not keep the Bilinear/automatic settings keys.'}
        foreach($control in @($passes,$strength,$preset,$style,$neuralPath,$nrRuntimeFolder,$nrRuntimeBrowse)){if($control.Enabled){throw 'Neural options stayed available with Neural Rendering off.'}}
        if($rowLabels[$passes].ForeColor -ne [Fm4Ui.Palette]::Dim){throw 'Greyed-out option label was not dimmed.'}
        $upscaler.SelectedIndex=3
        if($scale.Enabled -or $renderSummary.Text -notmatch 'Native:'){throw 'CAS sharpening was not presented as native rendering.'}
        Save-PagePreview $graphics 'Graphics-native-4K.png'
        # AI upscaling exposes the render resolution and reports what is drawn and reconstructed.
        # Start from automatic so the check does not depend on the user's saved render scale.
        $script:savedScale=0
        $upscaler.SelectedIndex=1
        if(-not $scale.Enabled -or $scale.SelectedIndex -ne 0 -or $renderSummary.Text -notmatch 'Render 1280 × 720; DLSS output 3840 × 2160'){throw "DLSS automatic summary is wrong: '$($renderSummary.Text)'."}
        $scale.SelectedIndex=2
        if($renderSummary.Text -notmatch 'Render 2560 × 1440; DLSS output 3840 × 2160'){throw "DLSS 1440p input summary is wrong: '$($renderSummary.Text)'."}
        $upscaler.SelectedIndex=0
        if($scale.Enabled -or (Read-Controls).internal_scale -ne 0){throw 'Native mode kept the DLSS render resolution.'}
        $mipBias.SelectedIndex=2;if((Read-Controls).runtime_overrides.texture_mip_bias -ne 1.0){throw 'Distant texture detail did not save its mip bias.'};$mipBias.SelectedIndex=1
        $savedProfile=$profile.SelectedIndex;$profile.SelectedIndex=1;$upscaler.SelectedIndex=5;$ss=Read-Controls
        if($scale.Enabled -or $ss.upscaler -ne 'Bilinear' -or $ss.internal_scale -ne 3 -or $renderSummary.Text -notmatch 'Supersampling: 3840 × 2160 to 2560 × 1440'){throw "Supersampling did not save as 4K native drawing: '$($renderSummary.Text)'."}
        $upscaler.SelectedIndex=0;$profile.SelectedIndex=$savedProfile
        $upscaler.SelectedIndex=2
        if($scale.SelectedIndex -ne 2 -or (Read-Controls).internal_scale -ne 2){throw 'The chosen render resolution was not restored for FSR.'}
        $profile.SelectedIndex=0
        if($renderSummary.ForeColor -ne [Fm4Ui.Palette]::Accent){throw 'An input larger than the output was not flagged.'}
        try{$null=Read-Controls;throw 'An input larger than the output was saved.'}catch{if($_.Exception.Message -notmatch 'must not be larger'){throw}}
        $profile.SelectedIndex=2
        # Neural Rendering path: RTX 50 uses the official path, RTX 40 the community path.
        $nr.Checked=$true;$neuralPath.SelectedIndex=0
        foreach($control in @($passes,$strength,$preset,$style,$neuralPath,$nrRuntimeFolder)){if(-not $control.Enabled){throw 'Neural options stayed greyed out with Neural Rendering on.'}}
        $fake=@(@{Name='AMD Radeon(TM) Graphics';DriverVersion='32.0.21025.1024'},@{Name='NVIDIA GeForce RTX 5080';DriverVersion='32.0.16.1742'})
        $plan=Resolve-Fm4NeuralPath -Requested auto -Adapters $fake
        if($plan.Path -ne 'official' -or $plan.Tested -or $plan.Summary -notmatch 'untested on RTX 50 hardware'){throw "RTX 50 auto selection is wrong: $($plan.Summary)"}
        $plan=Resolve-Fm4NeuralPath -Requested auto -Adapters @(@{Name='NVIDIA GeForce RTX 4080';DriverVersion='32.0.16.1742'})
        if($plan.Path -ne 'community' -or -not $plan.Tested -or $plan.Driver -ne [version]'617.42'){throw "RTX 40 auto selection is wrong: $($plan.Summary)"}
        $plan=Resolve-Fm4NeuralPath -Requested auto -Adapters @(@{Name='NVIDIA GeForce RTX 5070 Laptop GPU';DriverVersion='32.0.16.1088'})
        if($plan.Path -ne 'official' -or -not ($plan.Warnings -match '616\.64')){throw 'An old driver was not reported for the official path.'}
        $plan=Resolve-Fm4NeuralPath -Requested official -Adapters @(@{Name='NVIDIA GeForce RTX 4080';DriverVersion='32.0.16.1742'})
        if($plan.Path -ne 'official' -or $plan.Tested -or -not ($plan.Warnings -match 'meant for RTX 50')){throw 'Forcing the official path on RTX 40 was not explained.'}
        foreach($name in @('AMD Radeon RX 7900 XTX','NVIDIA GeForce GTX 1080')){
            $plan=Resolve-Fm4NeuralPath -Requested auto -Adapters @(@{Name=$name;DriverVersion='32.0.16.1742'})
            if($plan.Supported -or $plan.Path){throw "Unsupported GPU $name was given a Neural Rendering path."}
        }
        foreach($pair in @(@('NVIDIA RTX 5000 Ada Generation','ada'),@('Quadro RTX 4000','turing'),@('NVIDIA RTX PRO 6000 Blackwell Workstation Edition','blackwell'),@('NVIDIA GeForce RTX 4060 Ti','ada'),@('NVIDIA GeForce RTX 3080','ampere'))){
            if((Get-Fm4GpuGeneration $pair[0]).Key -ne $pair[1]){throw "GPU generation for $($pair[0]) is not $($pair[1])."}
        }
        $realAdapters=$displayAdapters
        try{
            $script:displayAdapters=$fake;$gpu.Text='';Update-GraphicsPage
            if($pathStatus.Text -notmatch 'RTX 5080.*Official NVIDIA path'){throw "Launcher did not show the RTX 50 official path: $($pathStatus.Text)"}
            Save-PagePreview $graphics 'Graphics-neural-RTX50-simulated.png' 404
            $script:displayAdapters=@(@{Name='NVIDIA GeForce RTX 4080';DriverVersion='32.0.16.1742'});$gpu.Text=' ';Update-GraphicsPage
            if($pathStatus.Text -notmatch 'RTX 4080.*Community compatibility path'){throw "Launcher did not show the RTX 40 community path: $($pathStatus.Text)"}
            Save-PagePreview $graphics 'Graphics-neural-RTX40.png' 404
            $script:displayAdapters=@(@{Name='AMD Radeon RX 7900 XTX';DriverVersion='32.0.21025.1024'});$gpu.Text=''
            if($pathWarning.Text -notmatch 'needs an NVIDIA RTX GPU' -or $pathWarning.ForeColor -ne [Fm4Ui.Palette]::Accent){throw 'An unsupported GPU was not flagged.'}
        }finally{$script:displayAdapters=$realAdapters;$gpu.Text=[string]$settings.gpu_adapter_name}
        $neuralPath.SelectedIndex=1
        if((Read-Controls).neural_path -ne 'official'){throw 'neural_path did not round-trip.'}
        $neuralPath.SelectedIndex=0
        # Options that depend on another switch are greyed out with it.
        $batch.Checked=$false
        if($gpuCompletion.Enabled -or $completionBoundary.Enabled){throw 'Batching-only options stayed available without batching.'}
        Save-PagePreview $advanced 'Advanced-batching-off.png' 360
        $batch.Checked=$true;if(-not $gpuCompletion.Enabled){throw 'GPU shader completion stayed greyed out.'}
        $wheelEnabled.Checked=$false;if($rotation.Enabled -or $fullRotation.Enabled){throw 'Wheel rotation options stayed available with the wheel off.'};$wheelEnabled.Checked=$true
        $ffb.Checked=$false;if($gain.Enabled -or $detail.Enabled -or $weight.Enabled -or $centering.Enabled -or $damping.Enabled -or $backend.Enabled){throw 'Force options stayed available with force feedback off.'}
        $ffb.Checked=$true;if(-not $gain.Enabled){throw 'Force options stayed greyed out.'}
        if($PreviewDirectory){Save-PagePreview $graphics 'Graphics-scrolled.png' 404;Save-PagePreview $advanced 'Advanced-scrolled.png' 360}
        $nr.Checked=$true;$passes.Value=3;$strength.Value=65;$profile.SelectedIndex=2;$upscaler.SelectedIndex=1;$scale.SelectedIndex=2;$frameRate.SelectedIndex=3
        $pfpUi.Enabled.Checked=$false;$pfpUi.Source.Text.Text='missing-but-disabled';$pfpUi.Mode.SelectedIndex=0;$pfpUi.Physics.SelectedIndex=0
        foreach($camera in $pfpUi.Camera){$camera.Inherit.Checked=$true}
        $nrRuntimeFolder.Text='custom-runtime-fixture'
        $adaptive.Checked=$true;$occlusionSamples.Value=750000;$bulkRegisters.Checked=$false;$gpuCompletion.Checked=$false;$cumulativeVisibility.Checked=$false;$completionBoundary.Checked=$false;$primarySubmit.Checked=$true
        $s=Read-Controls
        if(-not $s.neural_rendering -or $s.neural_passes -ne 3 -or $s.neural_strength -ne .65 -or $s.profile -ne '4K' -or $s.internal_scale -ne 2 -or $s.frame_rate_limit -ne 165 -or $s.neural_runtime_folder -ne 'custom-runtime-fixture'){throw 'Graphics control round trip failed.'}
        $detail.Value=40;$road.Value=55;$weight.Value=320;$centering.Value=10;$damping.Value=150;$forceMode.SelectedIndex=0;$invert.Checked=$true;$rotation.Value=1080;$fullRotation.Checked=$false
        Test-Fm4ControlsPage $controlsUi
        if($PreviewDirectory){$tabs.SelectedTab=$controlsUi.Tab;foreach($nav in $shell.NavButtons){$nav.Selected=($nav.Tag -eq $controlsUi.Tab)};Save-Fm4Preview $form (Join-Path $PreviewDirectory 'Controls-bound.png')}
        $wheel=Read-WheelControls
        if($wheel -notmatch '(?m)^adaptive_triggers=1\r?$' -or $wheel -notmatch '(?m)^backend=(auto|native|software)\r?$' -or $wheel -notmatch '(?m)^detail=0\.4\r?$' -or $wheel -notmatch '(?m)^steering_weight=3\.2\r?$' -or $wheel -notmatch '(?m)^road=0\.55\r?$' -or $wheel -notmatch '(?m)^mode=game\r?$' -or $wheel -notmatch '(?m)^centering=0\.1\r?$' -or $wheel -notmatch '(?m)^damping=1\.5\r?$' -or $wheel -notmatch '(?m)^invert=1\r?$' -or $wheel -notmatch '(?m)^rotation_degrees=1080\r?$' -or $wheel -notmatch '(?m)^full_rotation=0\r?$'){throw 'Wheel control round trip failed.'}
        $serialized=$s|ConvertTo-Json -Depth 12|ConvertFrom-Fm4Json
        if($serialized.neural_passes -ne 3 -or $serialized.neural_strength -ne .65 -or $serialized.runtime_overrides.query_occlusion_fake_sample_count -ne 750000 -or $serialized.runtime_overrides.gpu_bulk_type0_register_writes -ne $false){throw 'Settings JSON round trip failed.'}
        if($serialized.runtime_overrides.d3d12_readback_resolve_gpu_completion -ne $false){throw 'Shader completion control round trip failed.'}
        if($serialized.runtime_overrides.gpu_occlusion_query_cumulative_fallback -ne $false){throw 'Cumulative visibility control round trip failed.'}
        if($serialized.runtime_overrides.d3d12_readback_resolve_completion_boundary -ne $false){throw 'GPU completion boundary control round trip failed.'}
        if($serialized.runtime_overrides.d3d12_submit_on_primary_buffer_end -ne $true){throw 'Primary-buffer submit control round trip failed.'}
        if($serialized.neural_runtime_folder -ne 'custom-runtime-fixture' -or $serialized.frame_rate_limit -ne 165){throw 'Graphics settings JSON round trip failed.'}
        if($serialized.forza_plus.enabled -or $serialized.forza_plus.source_folder -ne 'missing-but-disabled' -or
            $serialized.forza_plus.prepared_game_folder -ne '.local/forza-plus/assets' -or $serialized.forza_plus.camera_fov.follow_low -ne $null) {throw 'PFP4 disabled settings did not round-trip safely with default path and inherited FOV.'}
        if($script:settings.runtime_overrides.query_occlusion_fake_sample_count -eq 750000){throw 'Reading controls modified the original runtime settings.'}
        $testRoot=Join-Path ([IO.Path]::GetTempPath()) ('fm4-launcher-selftest-'+[guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $testRoot -Force|Out-Null
        $originalSettingsPath=$script:settingsPath;$originalWheelPath=$script:wheelPath
        $originalInjection=$script:injectSecondCommitFailure
        try {
            $script:settingsPath=Join-Path $testRoot 'settings.json'
            $script:wheelPath=Join-Path $testRoot 'wheel.ini'
            $external=$defaults.Clone();$external.custom_setting='edited-while-open';$external.runtime_overrides=$external.runtime_overrides.Clone();$external.runtime_overrides.custom_runtime_key=29
            $external.forza_plus=@{enabled=$false;source_folder='missing-but-disabled';prepared_game_folder='.local/forza-plus/assets';update_folder=$null;mode='normal';physics_preset='stock';physics_target_fps=$null;camera_fov=@{follow_low=$null;follow_high=$null;hood=$null;bumper_high=$null;driver=$null;future_fov_setting=17};harder_ai_file=$null;future_launcher_setting='preserve-me'}
            $priorSettingsText=$external|ConvertTo-Json -Depth 12
            $priorWheelText="[wheel]`nenabled=1`n[custom]`nkeep=byte-exact`n"
            [IO.File]::WriteAllText($script:settingsPath,$priorSettingsText,[Text.UTF8Encoding]::new($false))
            [IO.File]::WriteAllText($script:wheelPath,$priorWheelText,[Text.UTF8Encoding]::new($false))
            $script:injectSecondCommitFailure=$false
            Save-Controls
            $saved=Get-Content -LiteralPath $script:settingsPath -Raw -Encoding UTF8 | ConvertFrom-Fm4Json
            if($saved.custom_setting -ne 'edited-while-open' -or $saved.runtime_overrides.custom_runtime_key -ne 29 -or $saved.neural_runtime_folder -ne 'custom-runtime-fixture' -or $saved.frame_rate_limit -ne 165 -or
                $saved.forza_plus.future_launcher_setting -ne 'preserve-me' -or $saved.forza_plus.camera_fov.future_fov_setting -ne 17 -or $saved.forza_plus.source_folder -ne 'missing-but-disabled') {throw 'External settings edits, unknown PFP4 fields or neural runtime folder were not merged and saved.'}
            $beforeFailureSettings=[IO.File]::ReadAllBytes($script:settingsPath)
            $beforeFailureWheel=[IO.File]::ReadAllBytes($script:wheelPath)
            $script:injectSecondCommitFailure=$true
            try {Save-Controls;throw 'Second-commit failure injection did not fail the save.'} catch {
                if($_.Exception.Message -notmatch 'previous settings were restored'){throw}
            }
            $afterSettings=[IO.File]::ReadAllBytes($script:settingsPath)
            $afterWheel=[IO.File]::ReadAllBytes($script:wheelPath)
            if([Convert]::ToBase64String($afterSettings) -ne [Convert]::ToBase64String($beforeFailureSettings) -or
               [Convert]::ToBase64String($afterWheel) -ne [Convert]::ToBase64String($beforeFailureWheel)){throw 'Failed paired save did not restore the exact original bytes.'}
            $restored=Get-Content -LiteralPath $script:settingsPath -Raw -Encoding UTF8 | ConvertFrom-Fm4Json
            if($restored.custom_setting -ne 'edited-while-open' -or $restored.runtime_overrides.custom_runtime_key -ne 29 -or $restored.neural_runtime_folder -ne 'custom-runtime-fixture' -or
                $restored.forza_plus.future_launcher_setting -ne 'preserve-me' -or $restored.forza_plus.camera_fov.future_fov_setting -ne 17) {throw 'Rollback did not retain custom settings and PFP4 advanced fields.'}
        } finally {
            $script:settingsPath=$originalSettingsPath;$script:wheelPath=$originalWheelPath;$script:injectSecondCommitFailure=$originalInjection
            $testRootResolved=[IO.Path]::GetFullPath($testRoot)
            $tempRootResolved=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
            if(-not $testRootResolved.StartsWith($tempRootResolved,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing test cleanup outside the temporary directory.'}
            Remove-Item -LiteralPath $testRootResolved -Recurse -Force -ErrorAction SilentlyContinue
        }
        Write-Output 'PASS: themed shell, sectioned pages with greyed-out options, native and upscaling labels, Neural Rendering path selection (RTX 50 official / RTX 40 community, simulated adapters), live control bindings (synthetic devices), settings controls, external-edit merge, and transactional rollback; no visible window or user settings changes.'
    }finally{$form.Dispose()}
    return
}
try{$result=$form.ShowDialog()}finally{$form.Dispose()}
if($result -eq 'OK'){
    if($settings.forza_plus.enabled){Message 'Forza Plus is unavailable in this build; turn it off to play FM4.';return}
    try {& (Join-Path $root 'tools/Run-Enhanced.ps1') -SettingsFile $settingsPath}
    catch {$null=[Windows.Forms.MessageBox]::Show(("FM4 launch failed: "+$_.Exception.Message),'FM4 launch failed')}
}

