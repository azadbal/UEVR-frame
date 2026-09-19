# Prototype 07: status, recovery and Hogwarts compatibility findings

## Current Hogwarts guidance (supersedes the test below)

The September 18 test failed compatibility/performance acceptance. Keep **Native
Stereo Fix on** and **both experimental optimizations off** for Hogwarts. The
user reports broken visuals and single-digit performance without the fix.
Do not repeat the Native Stereo Fix-off benchmark below; it is retained as the
historical test procedure. Supporting the fix's separate-eye rendering path is
a prerequisite for further Hogwarts optimization tests.

The surviving log covers 19:40:31.579–19:42:16.465; only one log.txt was available,
so the earlier launch with the fix already disabled cannot be independently
checked. Archived at
`build/diagnostics/prototype07-hogwarts-2026-09-18-195101/`.

Recorded settings and applied state agree with the user's second-run sequence:

| Local time | Event / applied result |
| --- | --- |
| 19:40:36.971 | Native Stereo Fix on; optimizations blocked, baseline output |
| 19:40:55.363 / .583 | Fix disabled; both eyes cropped and reduced |
| 19:41:25.751 / 28.184 | Pixel reduction disabled; both eyes projection-only |
| 19:41:42.187 / .590 | Projection crop disabled; baseline output |
| 19:41:51.070 / .427 | Projection crop enabled; both eyes projection-only |
| 19:41:56.670 / 57.861 | Pixel reduction enabled; both eyes reduced again |

Across stable sampled intervals, the submitted frame counter advances about
57.27/s with the fix on, 2.19/s with both experiments on and the fix off, 6.56/s
with projection only, 7.24/s with both experiments off, and 1.12/s after enabling
pixel reduction again. These are **frame-counter cadence observations**, not GPU
time measurements or independently measured headset FPS. They corroborate the
reported slowdown and its association with pixel reduction; they do not isolate
the CPU/GPU cause. Scene/settings were not independently controlled.

44 sampled submissions: 10 fix-on baseline, 5 fix-off baseline, 9 projection-only,
20 reduced in both eyes. No logged resolve rejection, suppressed/failed state,
or resource failure during these intervals. Thus the earlier latched recovery
failure is not logged here. Loading recovery was not established by this run.
No speedup is demonstrated. Next: map and instrument the Native Stereo Fix path
with its compatibility behavior retained, then measure where frame time goes
before another optimization benchmark. No further manual test requested now.

Read-only compatibility review identifies three contracts to address:

- Native Stereo Fix uses eye-local x=0 for both views; the current crop probe
  expects the right eye at x=width in a packed target.
- The fix renders into separate targets and packs them through pre-render copy
  commands; crop initialization/resolve currently requires a direct source.
- The fix copies OpenXR pipeline state to a subsequent queue slot; exact
  render/pose/probe association must be established for that path.

The smallest next engineering step is bounded, observation-only diagnostics at
these seams with Native Stereo Fix on and cropping off, plus CPU/GPU timing.
Removing the compatibility gate alone is insufficient. This code review does
not establish the cause of the slowdown.

Prototype 07 exercises the new requested-versus-applied status, transient frame
suppression/recovery, and persistent resource-failure reporting. The diagnostics
log settings, actual crop/reduction masks, frame association, rejection reason,
and reset state. It does not measure CPU/GPU frame time or establish a speedup.

## Package and initial conditions

Use the package at
`C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-07`.
Close any old game/injector, launch this package and the test game, then inject
with OpenXR, D3D12 and Native Stereo.

For the Hogwarts run, keep **Native Stereo Fix off for the entire test**. Keep
SceneView and SplitScreen compatibility off. Keep game quality, output
resolution, upscaler, frame geometry, scene and camera fixed. Turn the reported
DLSS 5 neural rendering option off for this first comparison, and keep frame
generation off. Hold any separate upscaling setting constant. Test neural
rendering on later as a separate comparison; its cost in this game's VR path has
not been measured. Turn on the volumetric frame and Performance
Diagnostics. The initial menu is only a setup/full-FPS observation, not a
benchmark.

## Historical manual Hogwarts run (do not repeat)

1. In the initial menu, confirm the frame appears and note that this observation
   is not benchmark data. Start or load a save and wait for normal gameplay.
2. Hold both **Experimental Projection Crop** and **Reduce Scene Pixels** off
   for 30 seconds in the same gameplay area.
3. Turn Projection Crop on and leave Reduce Scene Pixels off for 30 seconds.
   Check the sidebar status and logs for `Active - projection crop only`.
4. Turn Reduce Scene Pixels on for 30 seconds. Check the same scene, head
   movement and HUD alignment, then confirm status becomes `Active - reduced
   pixels in both eyes` when both eyes apply reduction. A frame may report
   projection crop only when reduction is requested but unavailable; record that
   transition and its reason.
5. Turn Reduce Scene Pixels off for 30 seconds, then on for 30 seconds, with
   every other setting unchanged. Report any visual change, hitch, blank frame,
   or status that remains waiting, partial, recovering, failed or blocked.
6. Load another save or reload the same save if practical, wait for the game to
   resume, and check whether a fresh valid frame returns to the expected active
   status. If any failure or blocked state appears, preserve and report it.
7. Exit the game normally. After the report, Codex can archive the
   UEVR log and filtered diagnostics under the prototype-07 run directory.

## Interpreting behavior

The expected status sequence is baseline while disabled, then active projection
crop only or active reduced pixels in both eyes as the pixel switch changes.
Status can briefly show waiting for a rendered frame or recovering when the
matching render/pose/projection data is unavailable. A frame whose modified
mapping cannot be matched is intentionally suppressed/cleared until a fresh
valid frame arrives; this transient blank does not by itself mean the resources
failed.

If the UI reports `Failed (rendering resources)` or the log reports a resource
allocation/resolve failure, treat it as persistent for that game session. Do not
claim recovery by toggling settings; restart the game before retrying and report
the failure and reason. Any `blocked` or `failed` state, unexpected status, or
repeated blank should be included with the surrounding log records.

Check stereo depth, sharpness, crop placement, reflections, HUD alignment,
flicker and head movement in each active interval. This run is a behavior and
appearance check with fixed settings. Do not claim full end-to-end correctness
or a measured performance gain from it. Headset validation of loading recovery
and the next runtime log review remain pending.

## Engineering checks

Release x64 build: PASS. Production crop/submission tests and the resolve recovery
policy test: PASS. The 11-case D3D12 WARP resolve regression also passes with no
debug-layer errors. Hogwarts loading recovery and UI appearance remain unverified
in-headset until the next run.

The production submission helpers are covered for missing association, wrapped
queue slots, reduced views without matching projections, pose replacement,
retained source identity and fresh-frame recovery. The resolve policy test
replays a valid crop, repeated lost/mask-zero submissions, and a fresh valid crop;
it also verifies that a resource failure stays latched until reset. These tests
do not reproduce the entire injected Hogwarts loading sequence.

Status is confirmed after the final mask step, with matching frame/generation.
Uncertain frames remain suppressed; their rejection no longer permanently clears
crop readiness. Resource/pipeline/RTV failures still require reset. The exact
upstream cause of the previous Hogwarts loss is not established; the new reason
and source identity diagnostics are intended to establish it in the next run.
