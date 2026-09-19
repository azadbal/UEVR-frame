# Prototype 04: aperture performance diagnostics

AZA-198, 2026-09-18. This build implements the observation stage of the
performance experiment. It does not narrow engine projections, reduce scene
resolution or allocations, or claim a speedup. Prototype 03 remains the
user-accepted baseline. Coupled resizing/virtual viewing distance (AZA-201)
and compact anchor/placement/HUD status (AZA-202) are deferred in Linear.

## What changed

The optional **Performance Diagnostics (no optimization yet)** toggle, default
off, captures the current anchored aperture at the first observed stereo
projection callback. Both eyes share that geometry, attached to OpenXR's pose
queue. The mask, linked HUD and submission use the associated geometry/bounds
when the rendered frame and output dimensions match. Missing, wrapped or
invalid snapshots retain the normal late mask path. The geometry builder is
the same function in both paths; no separate optimizer anchor was introduced.

Diagnostics record the engine matrices and view rectangles observed at the
hooks, conservative candidate per-eye crop rectangles with a 16-pixel guard,
frame/pose IDs, source/output sizes and relevant compatibility flags. Logging
is limited to a group every two seconds. The full image is still rendered;
candidate area is not a measured saving. This does not yet prove that Unreal's
effective culling view uses the projection returned by this hook.

Stage views and the head location must both report valid position/orientation
before accepting an early snapshot. Every pose update clears the old probe;
the requested render ID is checked against the queue slot's full pose ID.
Resolution changes invalidate the snapshot rather than reinterpreting it.

## Short FluidFlux check

1. Close the game and previous injector. Run `UEVRInjector.exe` from
   `C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-04`.
2. Launch `Game-demos/FluidFlux_3_0_1_Demo_UE532/FluidFlux.exe`, select
   `FluidFlux-Win64-Shipping`, and inject using the same working OpenXR/Native
   Stereo settings. Keep the volumetric frame enabled.
3. Open **Volumetric Frame (Experimental)** and enable **Performance
   Diagnostics (no optimization yet)**. Spend about 30 seconds looking ahead,
   leaning sideways, turning and tilting your head. Check that the frame/HUD
   placement and scene perspective behave as in prototype03.
4. Toggle diagnostics off/on once, then try Match Game UI and Recenter Frame.
   Report completion and any visible difference; Codex can read the local log.
   No file upload or manual DLL replacement is needed.

Log: `%APPDATA%/UnrealVRMod/FluidFlux-Win64-Shipping/log.txt`, prefix
`[Frame Perf]`. UEVR replaces this log on the next injection, so collect it
before another injection if investigating a failure.

## Reading the evidence

- `prepared=true matched=true`: the early snapshot reached this submission
  with matching frame ID and output dimensions.
- `projections=3`: both eye projection callbacks observed. `rects=3`: both
  view rectangle callbacks observed after the associated pose update. These
  are bit masks, not counts. Missing bits or persistent unmatched snapshots
  require investigating callback order before enabling cropping.
- `crop=x,y,width,height`: proposed region in one full-size eye, with no
  right-eye packing offset. `view=` is the observed engine view rectangle.
- Fallback codes: 0 candidate, 1 invalid input, 2 eye-plane crossing/behind,
  3 offscreen. Fallback candidates use the full eye.
- `P` prints all 16 engine matrix elements grouped in GLM storage columns.
  Neither the float nor double matrix returned to Unreal is modified.
- `geometry` records center, size and frozen runtime submission bounds.
  Only records with `matched=true` describe an accepted early snapshot.

The next gate is a real demo run with stable association for both eyes and
unchanged appearance. After that, implement opt-in narrowed projection and
crop-aware resolve together, keeping scene allocation unchanged initially.
Reduced active view sizes and allocations follow separately. Measure CPU/GPU
milliseconds only after visual equivalence, with diagnostic logging disabled.

## Local validation

- Release x64 `uevr` build succeeds.
- `tests/volumetric_frame_crop.cpp` exercises the production helper: both-eye
  oblique containment, asymmetric FOV/subimage mapping, guard/fallback, float
  and double crop/resolve pixel-center mapping, unchanged depth, and snapshot
  identity/dimension checks. Standalone VS2022 compile/run passes.
- Existing anchor regression passes.
- Existing D3D12 WARP mask harness passes all nine cases with zero pixel
  mismatches and no debug-layer errors. The mask shader is unchanged.
- At build time, headset verification was pending; no engine-culling result or
  performance measurement was available.

## First FluidFlux diagnostic run

2026-09-18, 18:20:57.965–18:21:52.161 local time. User reported completing the
headset check. Read the local log and archived its diagnostic records plus a
machine-readable summary under `build/diagnostics/prototype04-2026-09-18/`.

- 28 rate-limited submission samples: 26 matched the early snapshot with both
  eye projection and view-rectangle bits present (`projections=3 rects=3`).
- Two samples used the baseline path: initial `frame=0 pose=1`, and frame
  2190 at 18:21:16.012, where no early probe was prepared. The latter's cause
  is not identified by these records; a diagnostics-toggle transition is
  possible but not established. Later samples resumed matching.
- Full scene/output remained 5376 x 2880, or 2688 x 2880 per eye.
- Across the 26 matched stereo samples, mean left/right candidate area ranged
  from 33.84% to 49.70% of full stereo output, median 37.43%. All 52 matched
  eye candidates had fallback code 0. Regions include the 16-pixel guard.
- Geometry records show recenter placement changes and return to the matched
  UI rectangle; sampled projections and output dimensions stayed unchanged.
- One unrelated input-hook timeout was logged (`dinput8.dll` failed to load
  in time). No frame-mask initialization failure was logged during this run.

Interpretation: the normal both-eye capture/submission route works in this
demo's current configuration. These are sparse samples, not a per-frame
reliability measurement, image-equivalence proof, or benchmark. Roughly 63%
of output lies outside the median candidate rectangle; this only motivates
future pixel-work reduction, not a 63% frame-time/FPS prediction. The next
projection/resolve experiment must retain a per-frame full-view fallback,
including transitions with no prepared probe. Narrowing/culling and actual
CPU/GPU gains remain untested.
