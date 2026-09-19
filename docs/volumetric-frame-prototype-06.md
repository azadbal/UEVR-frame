# Prototype 06: reduce active scene views

Prototype 05 passed FluidFlux appearance testing: the user reported no perceptible
change when toggling projection cropping. Its recorded run contained 11 sampled
submissions with both eyes cropped, correct frame association, and no crop resolve
errors. This establishes appearance acceptance, not a measured performance gain.

Prototype 06 adds **Reduce Scene Pixels (Experimental)**, default off, beneath
**Experimental Projection Crop**. It asks Unreal for active view dimensions equal
to each eye's guarded crop. Scene allocations and headset output stay full-size;
the right eye retains its original packing offset. Reduced views resolve into
their original output positions with one source texel per output pixel. Existing
projection cropping without pixel reduction remains available for comparison.

## Manual FluidFlux check

Use `C:/Users/Azad/Documents/_apps/UEVR/volumetric-frame-prototype-06/UEVRInjector.exe`.
Close the old game/injector, launch this injector and FluidFlux, then inject as
usual with OpenXR, D3D12 and Native Stereo. Keep Native Stereo Fix, SceneView and
SplitScreen compatibility off, as in prototype 05. No manual DLL copying.

1. Enable the volumetric frame and Performance Diagnostics. Keep game quality,
   output resolution, upscaler and frame geometry unchanged throughout this test.
2. Start with both experiment switches off for about 15 seconds.
3. Enable Experimental Projection Crop, leaving Reduce Scene Pixels off, for
   about 15 seconds. This is the previously accepted full-size crop path.
4. Enable Reduce Scene Pixels for about 30 seconds. Look ahead, lean, turn and
   tilt; check stereo depth, sharpness, HUD alignment, water/reflections and
   flicker. Try Recenter Frame and black/green surroundings.
5. Toggle Reduce Scene Pixels off and back on. Report visible changes, including
   temporary blur or hitches. Quit the game normally; no need to disable
   diagnostics. Let Codex collect the log before another injection replaces it.

If the view goes blank or distorted, disable the experiment and report it. The
implementation deliberately blanks an already modified frame whose matching
projection/resolve data is lost rather than displaying it with an incorrect map.

After this appearance check, compare CPU/GPU frame times in a repeatable scene.
Hogwarts is a useful second target once this reduced-view path passes FluidFlux.
FPS alone may hide savings when already at the headset's refresh cap.

## Implementation and evidence

- Geometry is prepared before AdjustViewRect, using the rendered frame's pose.
  Each eye's returned rectangle and crop decision are frozen for that frame.
- Missing early pose or unsupported packing retains a full active view. Missing
  matching projection after a view reduction fails closed. Same-frame pose
  replacement also detects reductions before a projection has been returned.
- Resolve samples the recorded active rectangle, not the entire eye allocation.
  Equal source/output extents use an exact texel load; other cases retain sRGB
  bilinear filtering. The final aperture mask and separate HUD remain in place.
- Diagnostics add `reduced`: 0 means neither eye reduced, 1/2 one eye, 3 both.
  Candidate `view` dimensions show the actual returned active rectangles.
  `scene` and `output` are allocation dimensions and should remain unchanged.
- The full source-to-scratch copy remains. Allocation-wide passes, shadows,
  simulation and other shared work may remain unchanged. Smaller requested views
  do not prove that every engine pass shrank or that GPU time improved.

Validation: production crop/projection and phase 2/3 transition tests pass.
D3D12 WARP resolve covers 11 cases with no debug-layer errors, including both
reduced eyes, mixed full/reduced eyes and one-pixel edges. Equal-size mappings
are byte-exact; filtered interiors allow three channel values for GPU sampling
precision. Release build and runtime acceptance are recorded in the package
manifest. Headset behavior and measured performance remain pending.

Resize and virtual viewing distance remain deferred in AZA-201. Performance work
continues in AZA-198.

## First FluidFlux run: 2026-09-18

User completed the test and confirmed Reduce Scene Pixels looked the same,
including the requested sharpness/HUD/reflections/head-movement comparison.
This accepts appearance for this run; performance remains unmeasured.

- Log span: 18:53:42.584–18:54:30.423 local. Archived full log, filtered records
  and summary at `build/diagnostics/prototype06-2026-09-18-185518/`.
- 24 rate-limited submission samples: 8 baseline, 2 projection crop only,
  14 both-eye cropped and reduced. All 23 non-startup samples have matched
  frame/pose association, both projections and both view rectangles recorded.
- Reduced sampling begins at 18:54:04.326. All 28 reduced eye rectangles match
  their crop dimensions and retain the correct full-allocation packing offset.
  Combined active area: min 35.67%, median 39.23%, max 52.01% of baseline.
  The median is about 61% fewer requested active scene pixels, not a 61% speedup.
- Scene/output allocations remain 5376x2880 as intended (2688x2880 per eye).
  No logged errors after reduction begins and no crop resolve failure. Startup
  hook/initialization retries and a pre-crop DInput timeout are present.
- Samples show baseline → crop only → crop plus reduction. They do not establish
  off/on reversal, recenter or background-color coverage; those actions are not
  separately recorded here. Logs are sampled, not exhaustive per-frame evidence.

Next: heavier-game compatibility and controlled timing comparison in Hogwarts,
keeping frame placement, scene/camera, quality, upscaler and resolution fixed.
Compare the volumetric frame with both experimental switches off against both
on, then repeat off/on after warm-up. First check appearance in Hogwarts. Collect
CPU/GPU frame times with a verified measurement source before claiming gains;
these diagnostics currently report geometry, not CPU/GPU execution times.

## Hogwarts follow-up: 2026-09-18

User reports no visual artifacts and no perceived timing improvement: approximately
14–15 ms in the 72 Hz menu and 20 ms in gameplay. These are user-observed Virtual
Desktop readings, not timing measurements from our log.

Archived run: `build/diagnostics/prototype06-hogwarts-2026-09-18-191050/`,
19:06:05.367–19:08:55.653 local. Of 76 sampled submissions, 74 have Native Stereo
Fix enabled with cropped=0/reduced=0. Only two samples, 19:06:38.954 and
19:06:40.955, show native_fix=false/cropped=3/reduced=3. Their actual view
rectangles match the crop dimensions, occupying 35.29% and 37.68% of baseline
stereo pixel area. By 19:06:42.956 the compatibility fix is enabled again and
remains enabled in all later samples. No sustained in-game optimized comparison
is established; the logs do not identify the menu/gameplay boundary explicitly.

At 19:07:46.421 and 19:07:47.424, resolve reports frame 7228 lost=true/mask=0,
clears the output and latches cropping off until reset. Nearby logs show scene
capture reset, stalled engine tick and frame numbering restarting. This exposes
a lifecycle issue worth investigating separately from performance. No speedup
or performance regression is established by this run.

Before another benchmark, add explicit requested-versus-applied status and
transition logging, including the blocking reason and failure state. Diagnose
the lost-frame/reset sequence without weakening protection for in-flight cropped
images. Then compare sustained off/on intervals with Native Stereo Fix held off
and all other settings fixed; correlate actual activation with timing evidence.
