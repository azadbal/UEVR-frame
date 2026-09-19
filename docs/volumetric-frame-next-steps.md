# Volumetric frame: agreed prototype03 scope and next steps

Updated 2026-09-18 after user clarification. This supersedes the full-picture
camera-calibration and stereo-quad proposals in the earlier research notes.

## Priority 1: automatic frame and game HUD alignment

Preserve the existing native-stereo 6DoF world and post-render portal mask.
Ignore desktop/game FOV. Do not warp the world, change camera composition, or
switch scene presentation to UEVR's 2D screen path.

Use one flat 16:9 rectangle for the portal and, by default, the captured game HUD.
Derive its height, distance, horizontal/vertical offsets and normal initial pose
from UEVR's stage-anchored UI placement: standing origin, rotation offset and
decoupled-pitch settings. UI_Size is height in meters; width is height times
16/9. Activation, or changing Match Game UI from false to true, uses this normal
stage pose and never the instantaneous head pose.

The **Move Game UI with Frame** toggle is on by default. When on, the HUD shares
the frame's anchored rectangle and temporarily uses a flat quad. When off, the
HUD retains normal pose, size, head-follow behavior and cylinder mode while the
portal remains independently movable; it may extend outside the portal. If
UI_FollowView is enabled, no fixed normal screen exists, so automatic matching
uses a stable stage UI placement to keep the portal independent of head direction;
the saved setting is unchanged. UEVR's own settings panel remains separately
usable. Retain the original manual width/distance adjustment when matching is off.

**Recenter Frame** remains an explicit head-based action. It moves the frame, and
the HUD when Move Game UI with Frame is enabled, in front of the current head.

Matching placement/boundaries is the accepted goal; reproducing the exact desktop
camera composition is explicitly not required. A fixed 16:9 physical rectangle
does not itself relayout an ultrawide game canvas. Test with a 16:9 game window;
future aspect-ratio support must coordinate game UI canvas and portal geometry.

Acceptance: toggle on and see aligned frame/HUD without size/distance adjustment;
turn the head, then toggle Match Game UI on and verify the fixed normal stage UI
anchor; toggle Move Game UI off and manually resize/distance-check that the HUD
is unchanged; use explicit Recenter Frame to verify head-based placement; use
black or exact RGB(0,255,0) surroundings; toggle off and restore normal UI
behavior. Keep native 6DoF and the original FOV behavior; no desktop FOV or camera
projection changes and no performance optimization are part of this milestone.
UI drawn directly into the 3D scene cannot automatically be repositioned by
changing the captured HUD layer.

## Priority 2: optional reference FOV through physical geometry

Later allow a chosen horizontal FOV to set physical window width or distance at
a reference eye position, preserving the existing world/projection geometry:

- width = 2 * distance * tan(horizontal_FOV / 2)
- distance = width / (2 * tan(horizontal_FOV / 2))

For example, 90 degrees at 2 m distance means a 4 m-wide window. Keep height equal
to width times 9/16 and move/resize the HUD with the frame. The chosen FOV applies
at the reference position; leaning and approaching change the visible angle
naturally. Do not promise automatic discovery of each game's native FOV. This is
secondary scope, not part of the first alignment build.

## Parallel performance work

The user confirmed prototype 03 works in FluidFlux. The tested implementation is
preserved in commit `c473285` on `feature/volumetric-frame` and pushed to the user's
fork. Geometry acceptance no longer blocks the first performance experiment.

Astra investigates early per-eye aperture frusta and reduced scene dimensions
while Luna implements shared geometry. Integrate optimization after that geometry
is stable. The final mask remains necessary for exact oblique portal edges.

Preferred experiment for this agreed scope: preserve eye orientations, derive
conservative portal ray bounds per eye, render the corresponding smaller scene
view, then resolve it into the ordinary full-FOV OpenXR image. Match render-frame
pose, crop, projection and anchor through submission. Keep a guard band for
sampling/temporal effects. Do not recreate allocations with every head movement.

First prove cropped/uncropped ray mapping mathematically. Then add the engine
changes behind an opt-in comparison switch. Verify equivalence at unchanged
composition and visible sharpness before claiming gains. Narrowing FOV without
reducing scene pixels does not guarantee lower shading cost. Late masking alone
saves no scene work. Shadows, reflections, simulation and other shared work may
remain; a lightweight demo may be refresh-capped or simulation-bound.

## Test target and responsibilities

Primary target (user confirmed works with our existing build):
`C:/Dev/VR/UEVR/Game-demos/FluidFlux_3_0_1_Demo_UE532/FluidFlux.exe`.
Hogwarts is no longer required for the first iteration.

Agent work: source integration, Release x64 build, layout/math and D3D12 shader
checks, separate packaged injector, documented comparison controls. Preserve the
user's existing nightly and prototype packages.

User headset acceptance after packaging: launch
`C:/Dev/VR/UEVR/Game-demos/FluidFlux_3_0_1_Demo_UE532/FluidFlux.exe`, inject the
prototype03 package with OpenXR/Native Stereo, then run the Match Game UI,
Move Game UI and explicit Recenter Frame regression checks above. Also check
alignment/parallax and black/green surroundings. No need to repeat the already
confirmed old-build baseline now.

Performance acceptance later: repeatable demo scene and pose, fixed refresh,
resolution/upscaler and quality settings, warmed-up shaders; compare CPU/GPU frame
times and missed frames for identical portal output, optimization off/on. FPS
alone can hide gains at the headset refresh cap. No measured speedup yet.

## Evidence and tracking

Additional design investigations after the prototype 03 checkpoint:

- [Coupled window/world resizing](volumetric-frame-resize-design.md): a proposed
  size control for the apparent 3D scene and frame together, distinct from the
  existing aperture-only width and the optional reference-FOV control. No runtime
  implementation yet.
- [Other-fork comparison](volumetric-frame-fork-comparison.md): source-backed
  quality-of-life ideas from UEVR-6DOF-Window; no code imported.

- [2D/UI source tracing](volumetric-frame-2d-research.md): historical findings;
  composition-preserving camera proposal is superseded by this spec.
- [Rendering research](volumetric-frame-performance-research.md): historical
  alternatives; the physical-aperture branch is now selected.
- AZA-197: alignment implementation and validation.
- AZA-198: portal rendering experiment and FluidFlux measurements.
- AZA-136: headset acceptance, now using FluidFlux.
