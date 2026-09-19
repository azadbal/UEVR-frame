# Volumetric frame prototype 08

## Timed run

Source: `build/diagnostics/fluidflux-auto-2026-09-18-201919-837/log.txt`.

The automated FluidFlux path ran with native-fix enabled and crop/reduction
optimizations disabled. The `[Frame Timing]` records cover the
`uevr_openxr_native_submission` scope only; they are not whole-game timings.
There were eight timing intervals, with 1,154 GPU samples and 1,157 CPU
samples. The sample-weighted GPU means were:

- copy: 0.0731 ms
- mask: 0.0336 ms

All timing records reported `invalid=0`, `pending_skips=0`, and
`gpu_unavailable_images=0`. The measurements describe the native submission
path and do not establish GPU frame time, application FPS, or a Hogwarts
performance result.

The Native Stereo Fix optimization restriction remains in place: the run recorded
`crop_setting=false`, `reduce_setting=false`, and `native_fix=true`.

## Association trace

The trace shows a legitimate clone source pose/probe being used for the next
render frame. For example, target 2046 retained source pose/probe 2045 and pose
generation 330. The old validator rejected that target as queue-slot-mismatch
despite complete callbacks. The right-eye rectangle uses local x=0, because the
fix renders into separate textures. `scene=0x0` in submission logs means there is
no single direct source texture for this path; it does not mean empty rendering.

The fix records an explicit clone token at the actual copy site, retaining the
source pose identity and generation and the full target frame ID. Submission
validates those identities, complete eye callbacks, and unmodified views before
accepting the copied geometry. Ring reuse, missing association, replaced poses,
incomplete callbacks, and modified views remain rejected. The clone destination
also records its full frame ID rather than its queue index.

Final masking and submission bounds now use the captured source-frame geometry
for this known clone. This is a rendering behavior change; successful runtime
checks do not independently establish headset image quality or comfort. The
Native Stereo Fix crop gate and its eye-local rectangle eligibility stay unchanged.
Temporary detailed trace code was removed after capturing the diagnosis.

## Validation and package

Release x64 build passes. The production crop regression ran red on the recorded
2045→2046 case and green after the fix; it also covers the rejection cases above.
Three log-analyzer tests cover real positive/negative field layouts, numeric
masks, weighted means, interval maxima, and unavailable/zero-sample metrics.

Two consecutive live runs used Native Stereo Fix on, both optimizations off,
and the runner's `-RequireTiming -RequireMatched` checks:

| Archive suffix | Matched samples | GPU samples | GPU copy mean | GPU mask mean |
| --- | --- | --- | --- | --- |
| 202650-744 | 8/9 | 1,151 | 0.0757 ms | 0.0334 ms |
| 202748-418 | 8/9 | 1,154 | 0.0750 ms | 0.0334 ms |

Archives are under `build/diagnostics/fluidflux-auto-2026-09-18-*`. In each run,
the first sample after diagnostics re-enabling was unprepared; every subsequent
sample matched with both eye masks=3. No invalid GPU samples, unavailable timing
images, pending skips, or crop resolve rejections were recorded. Both tests closed
their game/injector and restored game/frontend configuration hashes. Existing
engine-hook fallback and DInput timeout messages remain; these passes are not a
claim that every log line is error-free.

Package: `C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-08`.
Backend SHA256: `93857CB8502243E560F11B3B89B8FEAAC0FFDA13E1F0DBF6E7C95946DAC8B1ED`.
The binary embeds the pre-change base commit; `PROTOTYPE-BUILD.txt` identifies the
final source checkpoint. No manual DLL replacement or new user test is needed.

```powershell
./tests/run-fluidflux-smoke.ps1 -Package 'C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-08' -FrontendConfig 'C:/Users/Azad/AppData/Local/praydog/UEVRInjector_Path_etdmgzpe34l1cv4o3ziw12ocs2pfiogn/1.0.0.0/user.config' -RequireTiming -RequireMatched
python tests/analyze_frame_diagnostics.py build/diagnostics/fluidflux-auto-2026-09-18-202748-418/log.txt --require-matched --json
```

Next work is separate-eye crop/view/resolve support and whole-game cost
measurement. Copy/mask timings alone cannot demonstrate a scene-rendering
optimization or explain Hogwarts' earlier slowdown. Real stereo image captures
and headset acceptance remain separate from these runtime checks.
