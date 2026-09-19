# Prototype 05: experimental projection crop and full-size resolve

Phase 2 is an opt-in narrowed stereo projection plus crop-aware resolve. The
scene resolution, output resolution, and scene allocations stay unchanged.
This is a visual/integration check; no performance gain has been measured.

## FluidFlux test setup

Use the package at:

`C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-05`

1. Close FluidFlux and any previous injector. Run `UEVRInjector.exe` from the
   package, launch `Game-demos/FluidFlux_3_0_1_Demo_UE532/FluidFlux.exe`,
   select `FluidFlux-Win64-Shipping`, and inject manually.
2. Use OpenXR, D3D12, and Native Stereo. Keep Native Stereo Fix, SceneView
   compatibility, SplitScreen compatibility, AFR, 2D Screen Mode, Extreme
   Compatibility Mode, and stereo emulation off.
3. Enable the existing volumetric frame. The experiment toggle is labeled
   **Experimental Projection Crop**. The diagnostics toggle is labeled
   **Performance Diagnostics**.

Do not copy or replace DLLs manually. Keep the same game quality, resolution,
temporal/upscaler, screen-space-effect, LOD, exposure, and other rendering
settings for every baseline/crop comparison. Projection cropping can change
screen-space effects, temporal history or upscaling, and LOD selection even
when the output dimensions are identical.

## Staged checks

1. **Full baseline:** with **Experimental Projection Crop** off, check scene
   perspective, head translation/parallax, HUD placement, Recenter Frame, and
   black/green surroundings. This is the comparison baseline.
2. **Opt-in crop:** enable **Experimental Projection Crop** and repeat the
   same viewpoint checks. Look ahead, lean sideways, turn, and tilt; then use
   Recenter Frame and verify the HUD remains matched to the full baseline.
3. Toggle the crop off and back on after the frame settles. Recheck the
   black/green surroundings and the full-baseline appearance. Note any
   fallback, transition, or visible change.
4. Enable **Performance Diagnostics** for this test. Codex can read the local
   log after completion; no upload is needed. Avoid another injection until
   the log is collected, as UEVR replaces it on injection.

Report visible differences with the exact toggle states and unchanged game
settings. Do not interpret candidate crop area as a speedup or claim a gain
from this run.

## Validation status

- Release x64 build: **PASS**.
- Production crop math, float/double projection mapping and per-eye eligibility
  checks: **PASS**. Existing anchor regressions and six Python crop tests pass.
- D3D12 WARP mask: all nine cases pass with zero pixel mismatches.
- New D3D12 WARP resolve: all seven cases pass, no debug-layer errors. Identity
  and background are byte-exact; filtered crop interiors match a CPU sRGB
  bilinear reference within three channel byte values (GPU filter precision).
- FluidFlux runtime appearance, actual culling savings and performance:
  **PENDING**. Synthetic shader tests do not verify game rendering behavior.
- Resize and virtual viewing distance remain deferred.

## Implementation and diagnostic evidence

The first requested frame stays full-view while resolve resources initialize.
After readiness, each eye's returned engine projection receives the crop only
when that eye's actual view rectangle matches the supported full-size packing.
The snapshot records which projections were changed; output uses those bits
even if the option is turned off before submission. Full-view fallback eyes
and cropped eyes can coexist without applying the wrong resolve to either.
The final exact aperture mask and separately captured HUD remain in place.

The runtime OpenXR FOV and eye poses stay unchanged. Only engine projection XY
changes; homogeneous depth and native head translation/rotation are preserved.
The shader copies the source scene into a scratch texture associated with each
swapchain image, then samples it into the original crop pixel rectangle. This
adds scratch memory and resolve bandwidth; scene allocation reduction is a
later phase. Color filtering decodes/encodes sRGB once. Eye-boundary clamps
prevent sampling the other eye. Depth submission is suppressed for cropped
frames because the depth texture is not resolved by this experiment.

`[Frame Perf] submit` now includes `cropped`: 0 is baseline, 1/2 is one cropped
eye, 3 is both. After initialization, ordinary centered views should report 3.
The existing `scene` and `output` dimensions should stay at baseline values.
Logs also identify pipeline/resource readiness and failures.

If an already cropped frame loses its association or usable resolve resources,
it shows the surrounding color instead of a distorted view and disables further
cropping until reset. If this occurs, switch the experiment off, report it, and
let Codex inspect the log before restarting. Do not treat a blank frame or
`cropped=0` throughout the run as a successful crop test.
