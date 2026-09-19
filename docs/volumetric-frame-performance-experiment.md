# Native 6DoF aperture performance experiment

2026-09-18. Design and standalone math validation only; no runtime optimization, source build, game run, or speedup measurement is included in this work.

## Target and boundaries

Preserve the current native 6DoF, full-FOV projection output with the physical aperture mask. Render fewer engine rays through a conservative rectangular bound of that aperture, then resolve them back into their original full-FOV pixel positions before the exact mask. Preserve head translation, rotation, eye separation, aperture placement, and black/green surroundings. Ignore desktop FOV. The stereo-quad/full-picture recommendation in the earlier [research note](volumetric-frame-performance-research.md) is historical and is not the selected experiment.

The known working local test target is `Game-demos/FluidFlux_3_0_1_Demo_UE532/FluidFlux.exe` (user-confirmed). Initial runtime scope: D3D12, OpenXR, ordinary synchronous stereo, existing supported aperture mode. Other rendering/compatibility modes retain the baseline until their view and allocation paths are verified. Physical aperture resizing/repositioning from a chosen FOV is separate priority-two work; it changes the visible opening and cannot count as an equivalent optimization.

Success means matching baseline scene rays and aperture/HUD placement, demonstrating that the narrower matrix reaches engine visibility construction, and measuring the resulting frame costs. A smaller theoretical pixel count alone is not success.

## Source-verified integration points

References are to this working tree; use the named functions when concurrent geometry changes move lines.

| Responsibility | Existing source | Required experiment change |
| --- | --- | --- |
| Pose/frame capture | [OpenXR::update_poses](../src/mods/vr/runtimes/OpenXR.cpp), [OpenXR::PipelineState](../src/mods/vr/runtimes/OpenXR.hpp) | Capture aperture geometry and crop decisions with the same eye poses used to construct this rendered frame; make it available before the first projection hook. |
| Scene projection | [FFakeStereoRenderingHook::calculate_stereo_projection_matrix](../src/mods/vr/FFakeStereoRenderingHook.cpp), currently around 5057/5177 | Keep the baseline near-plane/runtime matrix update, then apply a per-frame XY crop transform to the matrix returned to the engine. Handle float and double matrix writes. Do not mutate runtime submission FOV or its cached baseline projections. |
| Native 6DoF pose | Same file, `calculate_stereo_view_offset_`, around 4923 | Retain its ordinary head translation, rotation and eye separation. Do not enable the 2D-screen branch. |
| Compatibility matrix writes | Same file, `sceneview_constructor` around 3072 and `begin_render_viewfamily`/`do_splitscreen` around 3504 | These independently obtain projection matrices and write view rectangles. Route through the same crop decision before enabling these modes; otherwise they can replace the optimized matrix with a full projection. |
| Scene view size | Same file, `adjust_view_rect`, around 4686 | Later use distinct scene eye dimensions and packing, instead of `get_hmd_width/height`. Include the temporary skip-adjust safety paths. |
| Allocation/reallocation | Same file, `VRRenderTargetManager_Base::calculate_render_target_size` (6187), `need_reallocate_view_target` (6206 onward) | Later allocate/compare scene dimensions independently of output dimensions. A changed view rectangle alone does not establish smaller allocations. |
| Allocation compatibility | Same file, descriptor scans near 6672/6740/6840; `create_scene_capture` work near 7253 | Scans currently recognize full HMD double-wide dimensions; native stereo fix creates a separate eye target. Audit before enabling reduced allocations on these routes. |
| Full-size output | [D3D12Component.cpp](../src/mods/vr/D3D12Component.cpp), `OpenXR::create_swapchains`, around 1600 | Keep HMD-sized output swapchains. Do not globally change `get_hmd_width/height` to obtain smaller scene textures. |
| Resolve/mask | Same file, swapchain-copy block near 1904 and `draw_volumetric_frame` | Replace the scene copy for the supported color path with a crop-aware resolve, then run the exact mask. Existing `CopyResource` cannot resize/reposition a reduced scene. Audit pre/additional commands and any earlier scene copies/composition for assumptions that source and output dimensions match. |
| Submit association | [OpenXR::get_submit_state](../src/mods/vr/runtimes/OpenXR.cpp), around 692, and `end_frame`, around 1744/1812 | Resolve and mask consume the same rendered-frame snapshot. Keep submitted full-FOV eye pose, FOV and integer image rectangle unchanged. |

Epic's [stereo interface](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/IStereoRendering) identifies projection, eye offset and view-rectangle responsibilities, including a monoscopic projection encompassing all views. This establishes plausible seams, not proof of FluidFlux UE 5.3.2's actual culling path. The live documentation currently describes a newer engine. Verify the injected game's effective stereo/culling view before claiming geometry savings.

## Shared rendered-frame contract

Build on the shared aperture geometry used by the mask and HUD, rather than keeping another anchor in the optimizer. The required snapshot contains:

- Full frame identifier and geometry generation; reference-space/recenter generation; valid/enabled flags and a fallback reason.
- Aperture pose in stage space and physical half-size; both render-time eye poses and original runtime FOVs.
- Baseline engine projection per eye, including existing projection overrides; immutable integer output eye/submission rectangles derived with the exact submission rounding.
- Integer conservative crop rectangle in baseline full-eye pixels, derived narrow projection, actual scene view rectangle and allocation dimensions.

Resolve, mask, HUD and submission use that same snapshot. A shared geometry helper alone is insufficient if it is evaluated only at Present: engine visibility already happened. Finalize the crop before either eye's projection is returned, freeze it for both eyes, and carry it through `PipelineState` and render-frame association. Existing pose updates and frame copies must preserve the entire associated record. Snapshot mutable `view_bounds` too; recomputing them from latest globals at submission can disagree with the rendered crop.

If the earliest projection callback has no matching valid snapshot, choose the full scene for that frame and record the fallback. Never discover a missing crop at Present and reinterpret an already cropped image as a full image. Initial implementation should avoid changing the mode between projection and resolve, and must wait for in-flight resource users on mode/allocation transitions.

## Crop and resolve math

Use OpenXR eye space: +X right, +Y up, -Z forward. Tangents are `(l, r, t, b)` and UV has its origin at the top left. A full-view ray is

```text
ray(u,v) = (l + (r-l)u, t + (b-t)v, -1)
u = (x/(-z) - l)/(r-l)
v = (y/(-z) - t)/(b-t)
```

Transform the four aperture corners into each rendered eye's coordinates. With every corner safely in front of the eye, their projected min/max bounds enclose the convex projected opening. Expand outward by a guard band, floor minima, ceil maxima, and clamp to the output's visible eye region. Keep the exact post-mask because a tilted opening is generally not an axis-aligned rectangle. Initially fall back to the full view whenever a corner approaches/crosses the eye plane or coordinates are invalid. This avoids fragile division and near-plane clipping code. Completely empty crops may also fall back initially; skipping an eye's scene is a separate scheduling experiment.

**Two pixel mappings matter.** The mask constructs raw-runtime-FOV rays inside the integer submitted image rectangle. The engine may render a different full projection because of symmetry/mirror overrides. First project aperture bounds into that submitted rectangle and offset them into baseline full-eye pixel coordinates. Then derive the narrow engine projection from those pixel coordinates and the actual baseline engine projection. Do not apply raw runtime tangents directly as the new engine frustum under all projection overrides. Preserve the baseline mapping, including existing subimage rounding.

For crop `[x0,x1) x [y0,y1)` within a baseline full-eye image of `W x H`, set `u0=x0/W`, `u1=x1/W`, `v0=y0/H`, `v1=y1/H`. In column-vector clip-coordinate notation:

```text
x' = (x + (1-u0-u1)w)/(u1-u0)
y' = (y + (v0+v1-1)w)/(v1-v0)
z' = z
w' = w
```

Apply this after the baseline projection (`Pcrop = C * Pbaseline` in this notation). Confirm C++/GLM/UE multiplication conventions with known projected points; do not copy conceptual row indices into the engine blindly. Only XY changes, preserving homogeneous depth, near-plane behavior and reversed-Z. UE-added temporal jitter must be scaled consistently with the changed viewport: a jitter of one baseline output pixel becomes one pixel when scene dimensions equal the integer crop, but a different scene size requires corresponding scaling.

The resolve samples the cropped scene at

```text
sceneUV = ((outputPixelCenter.x-x0)/(x1-x0),
           (outputPixelCenter.y-y0)/(y1-y0))
```

inside the crop, accounting for its actual scene texture/eye packing. Initialize everything outside to the chosen surroundings color, then apply the unchanged exact mask. When scene width/height equal integer crop width/height, each full-output pixel center lands on a source texel center; this preserves the sampling lattice geometrically. If allocation is padded, distinguish active view dimensions from physical allocation dimensions and initialize the sampled border. Retain existing full-size OpenXR submission; pose/FOV and source image rectangle remain distinct parts of the [projection-view contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerProjectionView.html).

This is a proof of ray/pixel mapping, not identical final shading. Narrowed projections can affect LOD, temporal histories, exposure, screen-space effects and view-dependent culling. Those require runtime evidence.

## Bounded implementation phases

1. **Observe and establish the baseline.** Log per-frame geometry/crop candidates without changing rendering. Record effective scene projection, view rectangles, actual resource sizes, output rectangle, frame IDs and fallback reason. Compare mask corners and candidate bounds at lean, roll and recenter. Confirm the frame snapshot is available at the first engine projection call. Save a reproducible FluidFlux camera location and quality settings.

2. **First optimization knob: experimental aperture crop on/off, default off.** Narrow scene XY projection and add the full-size output resolve together. Retain current scene allocation and dimensions in this phase. Keep a fixed conservative guard band (start with 16 output pixels as a trial, not a proven temporal bound). This isolates early-frustum integration from resource sizing. Enable only when the shared snapshot and supported stereo path are valid. Expect possible higher scene pixel density and changed LOD; use this phase to establish culling and geometry, not as the final quality/performance comparison. Do not change the existing instance-occlusion compatibility setting.

3. **Shrink scene view rectangles, keep allocation stable.** Put cropped eye views in a stable double-wide allocation; use active view dimensions equal to the integer crop dimensions to retain baseline pixel density. The right-eye packing offset belongs to the scene allocation, not the output width. Map both eyes explicitly in the resolve. Verify actual view-sized passes shrink and histories/jitter remain correct. Allocation-wide work may remain.

4. **Shrink allocations once phase 3 is correct.** Split scene-eye size from output-eye size at all verified hooks. Begin with a fixed crop envelope for a stationary-head measurement, then consider coarse allocation buckets with active view rectangles inside them. Never silently clamp a growing crop into a smaller resource: render a documented larger bucket/full fallback after a safe reallocation, or defer enabling that crop. Measure transitions and history behavior before allowing head-motion-driven changes. UI textures and output swapchains remain sized according to their existing role; isolated HUD composition uses the shared aperture independently of scene UVs.

5. **Dynamic acceptance and performance.** Exercise both eyes through translation, rotation/roll, recenter, settings changes, reference-space changes, an aperture near/outside the headset FOV, one-eye visibility, approaching/crossing the plane, and menus/HUD. Preserve the baseline fallback for unsupported temporal/upscaler or stereo paths until observed correct. Broaden beyond FluidFlux only after this target passes.

## Verification and evidence

The pure-stdlib [crop math test](../tests/volumetric_frame_crop_math.py) runs with `python tests/volumetric_frame_crop_math.py`. On 2026-09-18, all six tests passed: 2,000 randomized asymmetric ray round trips; integer pixel-center crop/resolve equivalence; distinct runtime/engine FOV with integer subimage mapping; oblique aperture interior containment for both eyes; invalid/near-crossing full-view fallback; and unchanged homogeneous depth under XY crop. It is an executable mathematical specification, not a test of production hooks, culling, resource synchronization or HLSL.

For runtime verification, first use a fixed pose with temporal jitter/upscaling disabled and fixed exposure where available. Capture baseline and optimized full-size eye outputs and compute differences restricted to the aperture interior, excluding its antialiased boundary. Use visible landmarks and disparity to test rotation, translation, depth and scale. Then restore the user's normal effects and repeat moving-head tests; baseline and optimized settings must match within each comparison. HUD position and readability are separate acceptance checks. No blanket numeric image tolerance is justified until the baseline's own repeated-frame variability is measured.

Show that the matrix is present before scene visibility and inspect the effective culling view, including any shared stereo view. Where the build exposes them, collect visible/frustum-culled primitives and `stat initviews`, render-thread time, GPU time, draw/triangle counts and per-pass GPU timing. Epic documents both bounds-based culling and these [visibility statistics](https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine); availability in this packaged build still needs checking. A narrower per-eye matrix without reduced visibility workload is not evidence of successful early culling.

Compare baseline post-mask, crop with unchanged scene size, and crop with reduced scene size at the same aperture geometry and output resolution. Alternate trials after warm-up, record resolution/upscaler settings and actual dimensions, and report median/high-percentile CPU/GPU milliseconds. Include resolve/mask overhead and reallocation/history-reset hitches. Headset FPS can remain fixed at a refresh/reprojection threshold while GPU time changes; report both. None of these timings have been measured here.

FluidFlux water simulation, world tick, reflection captures, shadows, or other offscreen/shared work may dominate. That is a limitation to profile, not an observed diagnosis. A large water mesh intersecting the crop may remain visible. Cropped screen-space reflections, bloom, exposure and temporal reconstruction may require larger guard regions or make this method unsuitable for a particular configuration. The full-size resolve and exact mask still consume output bandwidth. No speedup follows from the standalone test or from projected crop area alone.
