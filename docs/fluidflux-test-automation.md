# Agent-run FluidFlux smoke test

The runner starts FluidFlux, launches the existing injector with
`--attach=FluidFlux-Win64-Shipping.exe`, and uses UEVR's built-in per-game Lua
autorun support to check the session. No backend rebuild or manual menu clicks
are required. A connected, awake Virtual Desktop headset remains a prerequisite.

```powershell
./tests/run-fluidflux-smoke.ps1
```

Defaults target this workstation's FluidFlux installation and prototype07.
`-Package`, `-Game`, and `-FrontendConfig` can override paths. The frontend config
is the **user.config for that injector package**, not the game's config.txt.
Its path was identified by observing which file that injector saved on exit;
do not assume the default hash applies to another package or computer.

The runner backs up both configs, selects OpenXR, keeps Native Stereo Fix on,
keeps both crop experiments off, and temporarily installs
`tests/fluidflux_smoke.lua` in the game's profile. The script toggles diagnostics
off/on through the API and checks readback. It does not change rendering modes.
Only processes started by this run are closed. Existing test games cause an
early refusal. Evidence is saved under `build/diagnostics/fluidflux-auto-*`.
Temporary script/result files are removed and config hashes verify restoration.

## What a pass means

- Lua reported OpenXR ready and HMD active for a 20-second observation interval.
- At least 100 engine ticks and callbacks for both UE5 view indices occurred.
- Runtime setting readback confirms fix on, both crop experiments off, and
  diagnostics restored on after the API toggle.
- At least three baseline submission observations have advancing frame IDs;
  OpenXR reported VISIBLE or FOCUSED during the session.
- Cleanup completed and both saved configurations were restored byte-for-byte.

This is an automation/runtime smoke test, **not** a stereo-image acceptance test,
GPU benchmark, or proof of correct frame-to-pose association. Those results are
explicitly separate in summary.json. The frame log is emitted before xrEndFrame;
it is evidence of submission-path activity, not a GPU presentation completion
counter. Per-eye image capture and scoped GPU timing remain future work.

## First live findings, September 18, 2026

Initial attempts identified the injector's independent OpenVR preference, the
Lua getter's required colon syntax, concurrent-log read sharing, and a transient
empty process path during startup. These harness issues were corrected.

The first passing run, `fluidflux-auto-2026-09-18-200617-683`, completed in about
27 seconds: 1,393 engine ticks, callbacks for both eyes, and nine advancing
baseline submission observations. Both configurations were restored.
The immediately repeated run, `fluidflux-auto-2026-09-18-200654-808`, also passed
in about 27 seconds with 1,384 ticks, both eye callbacks, nine submissions, and
both configurations restored. No FluidFlux process or test-owned injector was
left running. These are two consecutive unattended passes.

With Native Stereo Fix on, the current crop probe reports unprepared/unmatched
frames in FluidFlux. Accordingly smoke success does not depend on these probes,
and their matched count is recorded separately (zero in this run). This is a
concrete frame-association gap for subsequent Native Stereo Fix instrumentation;
it is not proof that the baseline stereo images are wrong or the cause of the
Hogwarts slowdown. An existing vtable-already-hooked warning also occurs and is
not automatically attributed to cropping, which stays off.

## Next automated experiment

Keep this runner as the launch/cleanup layer. Add bounded diagnostics for the
fix's two source targets, actual eye rectangles, and pose/render/probe identity.
Run and inspect those automatically in FluidFlux. Measure CPU scope duration and
GPU work separately: timers around UEVR copies/resolve measure that work only,
not all Unreal rendering, runtime pacing, or Virtual Desktop encoding. Whole-game
profiling still needs an appropriate measurement source.

Then implement compatibility and run warmed, fixed-scene baseline/experiment/
baseline comparisons with settings and applied states captured automatically.
Recorded/fixed poses and stereo texture snapshots would make geometry regressions
repeatable; physical headset comfort and final Hogwarts-specific acceptance still
need a short human check. FluidFlux results cannot establish Hogwarts performance.
