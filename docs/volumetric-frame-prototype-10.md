# Volumetric frame prototype 10

Prototype 10 adds an unattended, single-process benchmark for FluidFlux. The
runner applies a mode sequence in Lua, waits for OpenXR/HMD/world/pawn
readiness, warms each phase, records engine tick deltas, and saves per-phase
CSV samples and summaries. It backs up the game and frontend profiles, saves
the live log and configuration evidence, stops only owned processes, and
restores both profiles after success or failure.

The benchmark supports OpenXR resolution scales 1–3. The final package is:

```text
Package: C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-10
Backend SHA256: E66861477E01C7338334FDFF761537DD353E7B831201DEB241352855794726F2
```

The first reproduced run, `frame-benchmark-2026-09-18-215801-875`, used
`baseline,crop,reduced,crop,baseline`, 5-second warmups, and 12-second
measurements. The analyzer passed all five phases: 30 matched submission
probes, 1,999 valid scoped GPU samples, and full-eye dimensions of 8064 x
8640. The runner summary recorded a cleanup PID-path failure even though the
evidence analyzer passed; this run is retained as a diagnostic cleanup case.

The second reproduced run, `frame-benchmark-2026-09-18-220028-562`, used
`baseline,reduced,fixed,reduced,baseline`, 5-second warmups, and 15-second
measurements. It passed all five phases with 37 matched submission probes,
3,077 valid scoped GPU samples, restored configuration hashes, and the same
8064 x 8640 full-eye dimensions. The fixed phase used a view scale of 0.8 and
observed 6452 x 6913 active eye rectangles. That qualifies the requested
fixed-scale application and rectangle geometry; it does not qualify visual
sampling quality because image correctness and headset comfort were not
captured (`image_correctness_verified=false`).

Example command for the fixed-scale reproduction:

```powershell
./tests/run-frame-benchmark.ps1 -ResolutionScale 3 -VirtualDesktopFixOverride 1 -FixedScale 0.74 -WarmupSeconds 7 -MeasureSeconds 20 -ModeSequence 'baseline,reduced,fixed,reduced,baseline' -TimeoutSeconds 240
```

The reported engine deltas are timing samples from the game tick callback.
They can be summarized as engine cadence, but they are not GPU timings. The
`[Frame Timing]` records measure UEVR's OpenXR submission work and exclude
Unreal scene GPU work, runtime pacing, and initial scratch setup. They are
useful for checking copy/reconstruct/mask overhead and valid timestamp
availability, not for claiming whole-game FPS or a scene-rendering speedup.

## Final-build live results

All timings below are engine tick intervals in milliseconds, not game GPU time.
The headset remained connected through Virtual Desktop, with Native Stereo Fix
OFF. Profiles were restored byte-for-byte and owned game/injector processes
closed after each final passing run. Earlier failed cleanup/capture attempts
remain archived as failures. Initial FluidFlux runs used backend B1357601...;
the following two runs use the final hash above.

FluidFlux `frame-benchmark-2026-09-18-221436-680`, 3x, PASS:

| Phase | Median ms | p95 ms |
| --- | ---: | ---: |
| Baseline | 24.71 | 27.73 |
| Live-sized reduction | 19.38 | 30.25 |
| Fixed scale 0.74 | 19.31 | 22.25 |
| Live-sized repeat | 19.40 | 32.68 |
| Baseline repeat | 24.76 | 27.90 |

Fixed 0.74 renders approximately the same total active pixel count as the
observed live crop in this resting-headset pose. Per-eye sampling distribution
still differs; this is not proof of equal visual quality or the cause of stalls.
An earlier fixed-0.8 run gave 17.23/17.53 ms versus baseline 20.14/20.51 ms.
Cross-run baselines differ, so compare modes within each run. Crop-only in the
first 3x run regressed from baseline 20.13 to 22.67 ms; measured submission GPU
overhead increased from 0.929 to 2.057 ms. Cropping is not an automatic gain.

Deep Rock `frame-benchmark-2026-09-18-220739-587`, 1x, PASS, six phases:
baseline/crop/reduced/fixed/reduced/crop. Most medians were approximately 14 ms.
The second reduced phase had 816 ticks in 15 seconds (54.4/s), p95 45.50 ms;
first reduced and fixed had 1075/1080 ticks, p95 14.76/14.65 ms.
The final crop phase had 1078 ticks and p95 14.83 ms. The stalls already ended
before the final toggle: this is an intermittent episode, not proof that the
toggle caused recovery. Reduced submission GPU means stayed 0.106–0.108 ms,
and sampled crop extents moved only 0–1 pixels. The cause remains unlocalized.

DRG launches through Steam; direct EXE startup failed Steam initialization.
The agent dismissed Steam's theater introduction, enabled desktop spectator
view, closed UEVR's menu, pressed Enter, clicked Continue, and observed the
Space Rig before writing exactly `gameplay` (no newline) to the profile's
`data/codex-frame-benchmark-ready.txt`. The Lua script otherwise waits and
excludes menus. Existing Steam is never stopped by the runner. Allow extra
startup time with `-TimeoutSeconds 600` when controlling these menus.

`-CaptureFrames` requires resolution scale 1. All six DRG post-mask, doublewide
BMPs passed native queue/completion/mode/header/file validation. Capture uses
an existing fence, supports the actual BGRA8 typeless runtime backing format,
and restarts warmup after disk writing. Files remain under the game's
`diagnostics/` folder and are linked in analysis.json. Baseline/reduced/fixed
previews show consistent portal geometry and scene placement. Separate HUD
layers, motion artifacts, headset comfort and exact sampling quality remain
outside this still-image check. No automated image-equivalence claim is made.

The explicit `native_scaled` comparator is excluded from default sequences.
In FluidFlux, baseline `r.ScreenPercentage=0` means its effective scene scale
is unknown. Setting 75 caused severe slowdown; treating 75 as a downscale was
not a valid comparison. Dynamic-resolution operation mode reported 0 and
`r.GPUCsvStatsEnabled` was unavailable. Actual scene dimensions/whole-game GPU
timings need additional instrumentation before drawing a resolution conclusion.

## Next discriminating tests

Keep fixed source dimensions diagnostic/default-off. Repeat the intermittent
DRG episode with controlled pose changes, shared aligned dimensions and
separately fixed per-eye dimensions. Distinguish alignment, inter-eye size
asymmetry and temporal changes instead of presuming resource churn. Add
engine/render-thread or runtime timing to localize stalls outside submission.
Do not enable cropping with Hogwarts' required Native Stereo Fix path yet.

Validation: Release x64 build; production crop/view-state regression; 13
benchmark analyzer fixture tests; final FluidFlux and DRG live passes. The
analyzer rejects wrong modes, dimensions, visibility, delayed timing sources,
incomplete phases, failed captures and invalid BMPs. Tests assert evidence
validity, not that an optimization must outperform baseline.
