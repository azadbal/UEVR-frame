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
