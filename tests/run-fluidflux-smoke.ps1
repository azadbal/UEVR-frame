# Historical prototype07 smoke helper; use run-frame-benchmark.ps1 for current validation.
param(
    [string]$Package = 'C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-07',
    [string]$Game = 'C:/Dev/VR/UEVR/Game-demos/FluidFlux_3_0_1_Demo_UE532/FluidFlux.exe',
    [string]$FrontendConfig = 'C:/Users/Azad/AppData/Local/praydog/UEVRInjector_Path_vq1ugn2tgm4nwnngygjohieq3baiky3c/1.0.0.0/user.config',
    [ValidateRange(30, 180)][int]$TimeoutSeconds = 90,
    [switch]$RequireTiming,
    [switch]$RequireMatched
)
$ErrorActionPreference = 'Stop'
function Read-LiveText([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
$processName = 'FluidFlux-Win64-Shipping'
if (Get-Process -Name $processName,FluidFlux,HogwartsLegacy -ErrorAction SilentlyContinue) {
    throw 'Close running test games before starting an isolated FluidFlux run.'
}
$repo = Split-Path $PSScriptRoot -Parent
$runDir = Join-Path $repo ('build/diagnostics/fluidflux-auto-' + (Get-Date -Format 'yyyy-MM-dd-HHmmss-fff'))
$profile = Join-Path $env:APPDATA "UnrealVRMod/$processName"
$config = Join-Path $profile 'config.txt'
$scriptPath = Join-Path $profile 'scripts/codex-fluidflux-smoke.lua'
$resultPath = Join-Path $profile 'data/codex-fluidflux-smoke.txt'
$logPath = Join-Path $profile 'log.txt'
$injectorExe = Join-Path $Package 'UEVRInjector.exe'
foreach ($path in @($Game, $injectorExe, (Join-Path $Package 'UEVRBackend.dll'), $config, $FrontendConfig)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing required file: $path" }
}
if ((Test-Path -LiteralPath $scriptPath) -or (Test-Path -LiteralPath $resultPath)) {
    throw 'A previous smoke-test script/result exists; inspect and clean up that run first.'
}
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
Copy-Item -LiteralPath $config -Destination (Join-Path $runDir 'config.before.txt')
Copy-Item -LiteralPath $FrontendConfig -Destination (Join-Path $runDir 'frontend.before.config')
if (Test-Path -LiteralPath $logPath) { Copy-Item -LiteralPath $logPath -Destination (Join-Path $runDir 'log.before.txt') }
$settings = [ordered]@{
    Frontend_RequestedRuntime = 'openxr_loader.dll'
    VR_NativeStereoFix = 'true'
    VR_RenderingMethod = '0'
    VR_VolumetricFrame = 'true'
    VR_VolumetricFrameCrop = 'false'
    VR_VolumetricFrameReducePixels = 'false'
    VR_VolumetricFrameDiagnostics = 'true'
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
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fluidflux_smoke.lua') -Destination $scriptPath
    Write-Output "Run directory: $runDir"
    $injector = Start-Process -FilePath $injectorExe -WorkingDirectory $Package -ArgumentList '--attach=FluidFlux-Win64-Shipping.exe' -WindowStyle Hidden -PassThru
    $launcher = Start-Process -FilePath $Game -WorkingDirectory (Split-Path $Game -Parent) -PassThru
    while ($stopwatch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        if ($null -eq $ownedGame) {
            $candidate = Get-Process -Name $processName -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($candidate -and $candidate.Path -and $candidate.StartTime -ge $started) {
                $expected = Join-Path (Split-Path $Game -Parent) 'FluidFlux/Binaries/Win64/FluidFlux-Win64-Shipping.exe'
                if ([IO.Path]::GetFullPath($candidate.Path) -ne [IO.Path]::GetFullPath($expected)) {
                    throw 'Unexpected FluidFlux executable path; refusing to manage it.'
                }
                $ownedGame = $candidate
                Write-Output "Started FluidFlux PID $($ownedGame.Id)"
            }
        } elseif ($ownedGame.HasExited) { throw 'FluidFlux exited before the smoke test completed.' }
        if (Test-Path -LiteralPath $resultPath) {
            $result = Read-LiveText $resultPath
            if ($result -match '(?m)^state=failed') { throw $result }
            if ($result -match '(?m)^state=complete') {
                $log = Read-LiveText $logPath
                # Runtime smoke and geometry association are separate checks.
                $samples = @([regex]::Matches($log, '\[Frame Perf\] submit frame=(\d+)[^\r\n]*native_fix=true[^\r\n]*cropped=0 reduced=0'))
                $matchedSamples = @($samples | Where-Object {$_.Value -match 'matched=true projections=3 rects=3'}).Count
                if ($samples.Count -lt 3) { throw 'Insufficient baseline submission observations.' }
                if ($RequireMatched -and $matchedSamples -lt 3) { throw "Insufficient matched stereo geometry samples: $matchedSamples" }
                if ($result -notmatch '(?m)^view0=[1-9]\d*' -or $result -notmatch '(?m)^view1=[1-9]\d*') {
                    throw 'FluidFlux UE5 did not invoke both eye-view callbacks.'
                }
                $first = [long]$samples[0].Groups[1].Value
                $last = [long]$samples[-1].Groups[1].Value
                if ($last -le $first) { throw 'Rendered frame identifiers are not advancing.' }
                if ($log -notmatch 'XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 5|XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 6') {
                    throw 'OpenXR never reported VISIBLE or FOCUSED.'
                }
                if ($RequireTiming) {
                    $timingLines = @($log -split "`n" | Where-Object {$_ -match '\[Frame Timing\] scope=uevr_openxr_native_submission '})
                    foreach ($line in $timingLines) {
                        if ($line -match '\bgpu_n=(\d+)') { $gpuSamples += [int]$Matches[1] }
                        if ($line -match '\b(invalid|gpu_unavailable_images)=[1-9]\d*') { throw "Invalid GPU timing: $line" }
                    }
                    if ($gpuSamples -lt 100) { throw "Insufficient valid GPU timestamp samples: $gpuSamples" }
                }
                $passed = $true
                Write-Output "Runtime checks passed: settings readback, both eye callbacks, $($samples.Count) submissions ($first -> $last); matched crop probes=$matchedSamples. Cleaning up."
                break
            }
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $passed) { throw "Timed out after $TimeoutSeconds seconds waiting for injected VR smoke test." }
} catch {
    $failure = $_.Exception.Message
    Write-Output "FAIL: $failure"
} finally {
    $cleanupErrors = @()
    # A launch failure can occur before the shipping process has a readable Path.
    if ($null -eq $ownedGame -and $launcher) {
        $candidate = Get-Process -Name $processName -ErrorAction SilentlyContinue | Select-Object -First 1
        $expected = Join-Path (Split-Path $Game -Parent) 'FluidFlux/Binaries/Win64/FluidFlux-Win64-Shipping.exe'
        if ($candidate -and $candidate.Path -and $candidate.StartTime -ge $started -and
            [IO.Path]::GetFullPath($candidate.Path) -eq [IO.Path]::GetFullPath($expected)) { $ownedGame = $candidate }
    }
    foreach ($process in @($ownedGame, $launcher, $injector)) {
        try {
            if ($process -and -not $process.HasExited) {
                $null = $process.CloseMainWindow()
                if (-not $process.WaitForExit(3000)) { $process.Kill(); $null = $process.WaitForExit(5000) }
                if (-not $process.HasExited) { throw "Owned process $($process.Id) did not exit." }
            }
        } catch { $cleanupErrors += $_.Exception.Message }
    }
    foreach ($item in @(@($logPath,'log.txt'), @($resultPath,'smoke-result.txt'), @($config,'config.after.txt'))) {
        try {
            if (Test-Path -LiteralPath $item[0]) { Copy-Item -LiteralPath $item[0] -Destination (Join-Path $runDir $item[1]) }
        } catch { $cleanupErrors += $_.Exception.Message }
    }
    foreach ($item in @(@('config.before.txt',$config), @('frontend.before.config',$FrontendConfig))) {
        try { Copy-Item -LiteralPath (Join-Path $runDir $item[0]) -Destination $item[1] }
        catch { $cleanupErrors += $_.Exception.Message }
    }
    foreach ($path in @($scriptPath, $resultPath)) {
        try { if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path } }
        catch { $cleanupErrors += $_.Exception.Message }
    }
    $configRestored = (Get-FileHash $config).Hash -eq (Get-FileHash (Join-Path $runDir 'config.before.txt')).Hash
    $frontendRestored = (Get-FileHash $FrontendConfig).Hash -eq (Get-FileHash (Join-Path $runDir 'frontend.before.config')).Hash
    if ($cleanupErrors.Count -gt 0 -or -not $configRestored -or -not $frontendRestored) {
        $passed = $false
        $failure = "$failure Cleanup/restore failed: $($cleanupErrors -join '; ')"
    }
    [ordered]@{
        passed=$passed; failure=$failure; started=$started.ToString('o'); seconds=$stopwatch.Elapsed.TotalSeconds
        package=$Package; backend_sha256=(Get-FileHash (Join-Path $Package 'UEVRBackend.dll')).Hash
        config_restored=$configRestored; frontend_restored=$frontendRestored
        matched_crop_probe_samples=$matchedSamples; image_correctness_verified=$false
        scoped_gpu_timing_samples=$gpuSamples; whole_game_performance_measured=$false
    } | ConvertTo-Json | Set-Content (Join-Path $runDir 'summary.json')
    Write-Output "Evidence saved: $runDir"
    Write-Output "Final result: passed=$passed; config_restored=$configRestored; frontend_restored=$frontendRestored"
}
if (-not $passed) { exit 1 }
