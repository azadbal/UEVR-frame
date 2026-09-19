param(
    [string]$Package = 'C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-10',
    [string]$FrontendConfig = 'C:/Users/Azad/AppData/Local/praydog/UEVRInjector_Path_x2xdkjaolgeqyaix1mfwi02x3qa410oe/1.0.0.0/user.config',
    [ValidateSet('FluidFlux','DeepRock')][string]$GamePreset = 'FluidFlux',
    [ValidateRange(1,3)][int]$ResolutionScale = 3,
    [ValidateSet(0,1,2)][int]$VirtualDesktopFixOverride = 0,
    [ValidateRange(1,120)][int]$WarmupSeconds = 10,
    [ValidateRange(1,120)][int]$MeasureSeconds = 20,
    [ValidateRange(0.1,1)][double]$FixedScale = 0.8,
    [ValidateRange(25,100)][int]$NativePercentage = 75,
    [switch]$CaptureFrames,
    [string]$ModeSequence = 'baseline,crop,reduced,crop,baseline',
    [ValidateRange(30,1800)][int]$TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
if ($CaptureFrames -and $ResolutionScale -ne 1) { throw 'CaptureFrames requires ResolutionScale 1.' }
function Read-LiveText([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Find-OwnedGame {
    foreach ($candidate in @(Get-Process -Name $processName -ErrorAction SilentlyContinue)) {
        try {
            $candidate.Refresh()
            if ($candidate.HasExited) { continue }
            $candidatePath = $candidate.Path
            $candidateStarted = $candidate.StartTime
        } catch {
            # Startup/exit can briefly make process properties unavailable. Retry;
            # the final name-based assertion still fails for any surviving process.
            continue
        }
        if (-not $candidatePath) { continue }
        if ($candidateStarted -ge $started) {
            if ([IO.Path]::GetFullPath($candidatePath) -ne [IO.Path]::GetFullPath($expected)) {
                throw "Unexpected game executable path for PID $($candidate.Id)."
            }
            $candidate
        }
    }
}
function Stop-OwnedProcess($Process) {
    if ($null -eq $Process) { return }
    $Process.Refresh()
    if ($Process.HasExited) { return }
    $null = $Process.CloseMainWindow()
    if (-not $Process.WaitForExit(3000)) { $Process.Kill(); $null = $Process.WaitForExit(5000) }
    if (-not $Process.HasExited) { throw "Owned process $($Process.Id) did not exit." }
}
if ($GamePreset -eq 'DeepRock') {
    $Game = 'C:/Program Files (x86)/Steam/steamapps/common/Deep Rock Galactic/FSD.exe'
    $processName = 'FSD-Win64-Shipping'
    $expected = Join-Path (Split-Path $Game -Parent) 'FSD/Binaries/Win64/FSD-Win64-Shipping.exe'
} else {
    $Game = 'C:/Dev/VR/UEVR/Game-demos/FluidFlux_3_0_1_Demo_UE532/FluidFlux.exe'
    $processName = 'FluidFlux-Win64-Shipping'
    $expected = Join-Path (Split-Path $Game -Parent) 'FluidFlux/Binaries/Win64/FluidFlux-Win64-Shipping.exe'
}
if (Get-Process -Name FluidFlux,FluidFlux-Win64-Shipping,FSD,FSD-Win64-Shipping,HogwartsLegacy,UEVRInjector -ErrorAction SilentlyContinue) {
    throw 'A test game or injector is already running; refusing to disturb it.'
}
$segments = @($ModeSequence -split ',' | ForEach-Object { $_.Trim().ToLowerInvariant() })
if (-not $segments.Count -or ($segments | Where-Object { $_ -notin @('baseline','crop','reduced','fixed','native_scaled') })) {
    throw 'Modes must be baseline, crop, reduced, fixed, or native_scaled.'
}
$repo = Split-Path $PSScriptRoot -Parent
$runDir = Join-Path $repo ('build/diagnostics/frame-benchmark-' + (Get-Date -Format 'yyyy-MM-dd-HHmmss-fff'))
$profile = Join-Path $env:APPDATA "UnrealVRMod/$processName"
$config = Join-Path $profile 'config.txt'
$scriptPath = Join-Path $profile 'scripts/codex-frame-benchmark.lua'
$resultPath = Join-Path $profile 'data/codex-frame-benchmark.txt'
$logPath = Join-Path $profile 'log.txt'
$injectorExe = Join-Path $Package 'UEVRInjector.exe'
$python = (Get-Command python -ErrorAction Stop).Source
$analyzer = Join-Path $PSScriptRoot 'analyze_frame_benchmark.py'
foreach ($path in @($Game, $expected, $injectorExe, (Join-Path $Package 'UEVRBackend.dll'), $config, $FrontendConfig)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing required file: $path" }
}
$backendHash = (Get-FileHash (Join-Path $Package 'UEVRBackend.dll')).Hash
if (-not (Test-Path -LiteralPath $analyzer -PathType Leaf)) { throw "Missing analyzer: $analyzer" }
if ((Test-Path -LiteralPath $scriptPath) -or (Get-ChildItem -LiteralPath (Split-Path $resultPath -Parent) -Filter 'codex-frame-benchmark*' -File -ErrorAction SilentlyContinue)) {
    throw 'A previous smoke-test script/result exists; inspect and clean up that run first.'
}
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
Copy-Item -LiteralPath $config -Destination (Join-Path $runDir 'config.before.txt')
Copy-Item -LiteralPath $FrontendConfig -Destination (Join-Path $runDir 'frontend.before.config')
if (Test-Path -LiteralPath $logPath) { Copy-Item -LiteralPath $logPath -Destination (Join-Path $runDir 'log.before.txt') }
$settings = [ordered]@{
    Frontend_RequestedRuntime = 'openxr_loader.dll'
    VR_NativeStereoFix = 'false'
    OpenXR_ResolutionScale = $ResolutionScale.ToString('0.0',[Globalization.CultureInfo]::InvariantCulture)
    OpenXR_VirtualDesktopFixOverride = $VirtualDesktopFixOverride
    VR_VolumetricFrameFixedViewScale = '0'
    VR_RenderingMethod = '0'
    VR_VolumetricFrame = 'true'
    VR_VolumetricFrameCrop = 'false'
    VR_VolumetricFrameReducePixels = 'false'
    VR_VolumetricFrameDiagnostics = 'true'
    VR_VolumetricFrameCapture = 'false'
    VR_Compatibility_SceneView = 'false'
    VR_Compatibility_SplitScreen = 'false'
}
$content = [IO.File]::ReadAllText($config)
foreach ($entry in $settings.GetEnumerator()) {
    $pattern = '(?m)^' + [regex]::Escape($entry.Key) + '=[^\r\n]*'
    $line = $entry.Key + '=' + $entry.Value
    if ([regex]::IsMatch($content, $pattern)) { $content = [regex]::Replace($content, $pattern, $line) }
    else { $content += "`r`n$line" }
}
$ownedGame = $null
$launcher = $null
$injector = $null
$passed = $false
$failure = $null
$matchedSamples = 0
$gpuSamples = 0
$lastStatus = ''
$started = Get-Date
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
try {
    [xml]$frontendXml = [IO.File]::ReadAllText($FrontendConfig)
    foreach ($pair in @(@('OpenXRRadio','True'), @('OpenVRRadio','False'))) {
        $node = $frontendXml.SelectSingleNode("//setting[@name='$($pair[0])']/value")
        if ($null -eq $node) { throw "Missing frontend setting $($pair[0])" }
        $node.InnerText = $pair[1]
    }
    $frontendXml.Save($FrontendConfig)
    [IO.File]::WriteAllText($config, $content)
    Copy-Item -LiteralPath $config -Destination (Join-Path $runDir 'config.test.txt')
    New-Item -ItemType Directory -Path (Split-Path $scriptPath -Parent) -Force | Out-Null
    $modeLua = ($segments | ForEach-Object { "'$_'" }) -join ','
    $scaleLua = $FixedScale.ToString('R',[Globalization.CultureInfo]::InvariantCulture)
    $captureLua = if ($CaptureFrames) { 'true' } else { 'false' }
    $preamble = "local config = {modes={$modeLua}, warmup_seconds=$WarmupSeconds, measure_seconds=$MeasureSeconds, fixed_scale=$scaleLua, native_percentage=$NativePercentage, game='$GamePreset', capture_frames=$captureLua}"
    $lua = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'frame_benchmark.lua'))
    if (-not $lua.Contains('-- BENCHMARK_CONFIGURATION')) { throw 'Missing Lua configuration marker.' }
    [IO.File]::WriteAllText($scriptPath,$lua.Replace('-- BENCHMARK_CONFIGURATION',$preamble))
    Copy-Item -LiteralPath $scriptPath -Destination (Join-Path $runDir 'benchmark.lua')
    Write-Output "Run directory: $runDir"
    $injector = Start-Process -FilePath $injectorExe -WorkingDirectory $Package -ArgumentList "--attach=$processName.exe" -WindowStyle Hidden -PassThru
    if ($GamePreset -eq 'DeepRock') {
        # Let the existing Steam session supply Steamworks context. Do not own or stop Steam.
        Start-Process -FilePath 'C:/Program Files (x86)/Steam/steam.exe' -ArgumentList '-applaunch', '548430', '-dx12' -WindowStyle Hidden
    } else {
        $launcher = Start-Process -FilePath $Game -WorkingDirectory (Split-Path $Game -Parent) -PassThru
    }
    while ($stopwatch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        if ($null -eq $ownedGame) {
            $candidate = Find-OwnedGame | Select-Object -First 1
            if ($candidate) {
                $ownedGame = $candidate
                Write-Output "Started game PID $($ownedGame.Id)"
            }
        } elseif ($ownedGame.HasExited) { throw 'Game exited before benchmark completion.' }
        if (Test-Path -LiteralPath $resultPath) {
            $result = Read-LiveText $resultPath
            if ($result -match '(?m)^state=failed') { throw $result }
            $status = (($result -split "`n" | Where-Object { $_ -match '^(state|segment|mode)=' }) -join ' ').Trim()
            if ($status -ne $lastStatus) { Write-Output $status; $lastStatus = $status }
            if ($result -match '(?m)^state=complete') {
                $passed = $true
                break
            }
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $passed) { throw "Timed out after $TimeoutSeconds seconds waiting for injected VR benchmark." }
} catch {
    $failure = $_.Exception.Message
    Write-Output "FAIL: $failure"
} finally {
    $cleanupErrors = @()
    # Stop launch sources first, then discover children again after they cannot
    # create another shipping process. Every cleanup failure still permits restore.
    foreach ($process in @($launcher, $injector, $ownedGame)) {
        try { Stop-OwnedProcess $process } catch { $cleanupErrors += $_.Exception.Message }
    }
    if ($launcher -or $GamePreset -eq 'DeepRock') {
        for ($attempt = 0; $attempt -lt 3; $attempt++) {
            try {
                foreach ($process in @(Find-OwnedGame)) { Stop-OwnedProcess $process }
            } catch { $cleanupErrors += $_.Exception.Message }
            Start-Sleep -Milliseconds 300
        }
    }
    $processesStopped = $true
    try {
        $residualWait = [Diagnostics.Stopwatch]::StartNew()
        do {
            $remaining = @(Get-Process -Name $processName,(Split-Path $Game -LeafBase),UEVRInjector -ErrorAction SilentlyContinue)
            if (-not $remaining.Count) { break }
            Start-Sleep -Milliseconds 250
        } while ($residualWait.Elapsed.TotalSeconds -lt 5)
        if ($remaining.Count) { throw "Processes remain after cleanup: $($remaining.Id -join ', ')" }
    } catch { $processesStopped = $false; $cleanupErrors += $_.Exception.Message }
    foreach ($item in @(@($logPath,'log.txt'), @($resultPath,'benchmark-result.txt'), @($config,'config.after.txt'))) {
        try {
            if (Test-Path -LiteralPath $item[0]) { Copy-Item -LiteralPath $item[0] -Destination (Join-Path $runDir $item[1]) }
        } catch { $cleanupErrors += $_.Exception.Message }
    }
    foreach ($item in @(@('config.before.txt',$config), @('frontend.before.config',$FrontendConfig))) {
        try { Copy-Item -LiteralPath (Join-Path $runDir $item[0]) -Destination $item[1] }
        catch { $cleanupErrors += $_.Exception.Message }
    }
    foreach ($artifact in @(Get-ChildItem -LiteralPath (Split-Path $resultPath -Parent) -Filter 'codex-frame-benchmark*' -File -ErrorAction SilentlyContinue)) {
        try {
            Copy-Item -LiteralPath $artifact.FullName -Destination $runDir
            Remove-Item -LiteralPath $artifact.FullName
        } catch { $cleanupErrors += $_.Exception.Message }
    }
    foreach ($path in @($scriptPath)) {
        try { if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path } }
        catch { $cleanupErrors += $_.Exception.Message }
    }
    $configRestored = $false
    $frontendRestored = $false
    try { $configRestored = (Get-FileHash $config).Hash -eq (Get-FileHash (Join-Path $runDir 'config.before.txt')).Hash }
    catch { $cleanupErrors += $_.Exception.Message }
    try { $frontendRestored = (Get-FileHash $FrontendConfig).Hash -eq (Get-FileHash (Join-Path $runDir 'frontend.before.config')).Hash }
    catch { $cleanupErrors += $_.Exception.Message }
    if ($cleanupErrors.Count -gt 0 -or -not $configRestored -or -not $frontendRestored) {
        $passed = $false
        $failure = "$failure Cleanup/restore failed: $($cleanupErrors -join '; ')"
    }
    $summary = [ordered]@{
        passed=$passed; failure=$failure; started=$started.ToString('o'); seconds=$stopwatch.Elapsed.TotalSeconds
        game=$Game; scale=$ResolutionScale; vd_fix=$VirtualDesktopFixOverride; segments=$segments
        fixed_scale=$FixedScale; native_percentage=$NativePercentage; warmup_seconds=$WarmupSeconds; measure_seconds=$MeasureSeconds
        capture_frames=[bool]$CaptureFrames
        package=$Package; backend_sha256=$backendHash; processes_stopped=$processesStopped
        config_restored=$configRestored; frontend_restored=$frontendRestored
        matched_crop_probe_samples=$matchedSamples; image_correctness_verified=$false
        scoped_gpu_timing_samples=$gpuSamples; whole_game_performance_measured=$false
    }
    $summary | ConvertTo-Json | Set-Content (Join-Path $runDir 'summary.json')
    try {
        & $python $analyzer $runDir
        if ($LASTEXITCODE -ne 0) { throw 'Per-phase benchmark evidence validation failed; see analysis.json.' }
        $analysis = Get-Content -LiteralPath (Join-Path $runDir 'analysis.json') -Raw | ConvertFrom-Json
        $summary.matched_crop_probe_samples = $analysis.submit_samples
        $summary.scoped_gpu_timing_samples = $analysis.gpu_samples
    } catch {
        $passed = $false
        $failure = "$failure $($_.Exception.Message)".Trim()
    }
    $summary.passed = $passed
    $summary.failure = $failure
    $summary | ConvertTo-Json | Set-Content (Join-Path $runDir 'summary.json')
    Write-Output "Evidence saved: $runDir"
    Write-Output "Final result: passed=$passed; config_restored=$configRestored; frontend_restored=$frontendRestored"
}
if (-not $passed) { exit 1 }
