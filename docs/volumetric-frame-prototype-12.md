# Volumetric frame prototype 12

AZA-205: projection crop with Native Stereo Fix enabled. Pixel reduction remains
in place and remains blocked with Native Stereo Fix. Its removal is deferred by
the user's latest instruction. This is a compatibility candidate, not a measured
performance result.

## Rendering change

Native Stereo Fix renders into separate eye-local targets, both with x=0. The
probe now records that packing per pose. The existing native copy path assembles
the two images into the double-wide XR image, which is preserved in scratch
before clearing and reconstructing the portal. Source rectangles are converted
to packed coordinates only for that reconstruction.

Known native frame clones may carry crop-only geometry when their source pose,
generation, complete view records and crop decisions remain valid. Stale or
incompatible associations retain fail-closed behavior. Pixel reduction is blocked
both in the requested settings and in the native probe's view-rectangle policy.
Ordinary stereo cropping retains its existing source route.

Crop-only retains full scene view dimensions, concentrating samples into the
portal. It can look sharper. A narrower view can reduce visibility work, but the
scene still shades a full-sized image and reconstruction has a cost. Speedups are
not guaranteed; source review or crop area is not a performance measurement.

## Existing engine statistics

UEVR's Show Engine Statistics option (VR_ShowStatsOverlay) calls Unreal's
`stat unit`; Show FPS calls `stat fps`. The benchmark runner can enable the
statistics overlay with `-ShowEngineStats` and restores the original profile.
Without that switch it preserves the profile's overlay setting. This does not
yet export the overlay's numeric values to benchmark CSVs. Existing engine tick
deltas and scoped UEVR GPU timestamps remain distinct measurements; neither is
whole-scene GPU time. Which statistics the shipping game exposes needs a live
check.

## Build and checks

Release x64 build passed. Backend SHA256:

`65FEB9F74B6F3938E314A61ADC0AE6DE8A856628D2B9A3971F02D1A2F5D5852F`

Separate package:
`C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-12`

- Native eye-local regression failed before the change, then passed. It covers
  packed-eye mapping, clone crop decisions, source-generation mismatch, native
  reduction rejection and changed rectangles, alongside previous OFF-path tests.
- D3D12 WARP: all 13 shader/copy cases passed pixel comparisons and debug-layer
  validation, including native separate-eye assembly and black/green output.
  This fixture validates copy ordering and the production shader, not the live
  engine resource/frame association.
- 30 Python checks passed, including native flag mismatch rejection, strict
  assembled-source/output dimensions, and matching runtime timing reports.
- PowerShell syntax and git whitespace checks passed.
- Independent renderer review found no concrete blocker.

## Runtime status and next test

No live prototype-12 run completed yet. The first attempted runner invocation
stopped at its existing-process guard: two pre-existing elevated prototype-11
injectors were still open. This Codex shell is not elevated. No game was launched
or profile modified by that attempt; the owned prototype-12 setup injector was
closed. The user was asked to close the older injectors. Do not label this build
runtime-validated or claim new performance gains.

Once the older injectors are closed, run:

```powershell
& tests/run-frame-benchmark.ps1 `
  -Package 'C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-12' `
  -FrontendConfig 'C:/Users/Azad/AppData/Local/praydog/UEVRInjector_Path_u3t05ixrdp3lpuyui50jns4nnipelbwa/1.0.0.0/user.config' `
  -NativeStereoFix -ShowEngineStats -ResolutionScale 1 `
  -VirtualDesktopFixOverride 1 -WarmupSeconds 8 -MeasureSeconds 20 `
  -ModeSequence 'baseline,crop,baseline' -CaptureFrames -TimeoutSeconds 240
```

The runner now accepts only baseline/crop modes with Native Stereo Fix ON and
verifies its setting per phase. Captures remain separate from measurement windows.
After successful source-association and image checks, test SamePass ON/OFF, the
existing native-fix OFF path, recenter/resize and recovery. Use FluidFlux 3x for a
GPU-heavier comparison only after correctness. Hogwarts retains Native Stereo Fix
ON throughout its subsequent test. Moving-head stereo, HUD alignment and comfort
still require headset observation.
