# Volumetric frame: rendering performance research

Scope update: the user selected the physical-aperture branch, preserving current
native 6DoF and ignoring desktop FOV. The full-picture stereo-quad alternative
below is historical, not selected. See [the agreed spec](volumetric-frame-next-steps.md).

Research only, 2026-09-18. Source inspection and API contracts below are verified; feasibility, visual quality, and performance predictions are untested in a game/headset. No renderer implementation was changed. Local line references describe this working tree and may move.

## Recommendation

There are two distinct targets. For **the existing stereo 2D screen's full picture, with head-motion parallax added**, first prototype dynamic per-eye off-axis cameras on the existing eye-specific quad path. For **a physically faithful opening into the current VR world**, narrow the engine's per-eye frusta to the aperture, reduce actual scene render dimensions, and preserve the mapping into OpenXR. Both can render stereo depth; their neutral-view composition need not match.

Do not promise an FPS multiplier. The current mask is applied after scene rendering, so it cannot save that work. Real savings require changes before visibility determination and expensive scene passes. Compare an optimized portal against an unoptimized portal with identical composition and quality, not against a differently framed full-VR image.

## Verified local seams

| Responsibility | Local evidence | Implication |
| --- | --- | --- |
| Current final-image mask | `src/mods/vr/D3D12Component.cpp:1904` copies the image; `:1930` then calls the mask | Geometry, shading, and most post-processing have already happened. |
| Frame placement | `src/mods/vr/D3D12Component.cpp:114` uses submitted eye poses; `:116` anchors at recenter; `:149` sets aperture size | Physical aperture geometry can be reused, but currently becomes available too late for engine culling. |
| Per-eye engine projection | `src/mods/vr/FFakeStereoRenderingHook.cpp:5057`, `:5177`, `:5185` | Existing hook supplies the engine with runtime projection matrices. This is a plausible early culling seam. |
| Eye translation and rotation | `src/mods/vr/FFakeStereoRenderingHook.cpp:4923` through `:4968` | Existing screen mode omits head translation/rotation while retaining eye separation; simply enabling translation is insufficient without matching off-axis projection. |
| Scene viewport/allocation | `src/mods/vr/FFakeStereoRenderingHook.cpp:4686`, `:6187` | Controls per-eye rectangle and double-wide render dimensions. |
| Compatibility paths | `src/mods/vr/FFakeStereoRenderingHook.cpp:3127`, `:3554` | Direct SceneView rectangle/projection writes must agree with the primary hooks. |
| Projection construction/cache | `src/mods/vr/runtimes/OpenXR.cpp:485`, `:548`, `:558` | Asymmetric, reversed-Z projection already exists; pose-dependent portal bounds must update each rendered frame rather than only on current projection-cache invalidations. |
| Frame association | `src/mods/vr/runtimes/OpenXR.hpp:212`; `OpenXR.cpp:1744` | Store aperture pose, render frusta, and crop rectangles with the rendered frame, not in mutable latest-state globals. |
| Submitted projection | `src/mods/vr/runtimes/OpenXR.cpp:1812` through `:1832` | Current submission uses original eye pose/FOV and cropped texture rectangles. A source crop alone does not define display placement. |
| Existing stereo screen | `src/mods/vr/D3D12Component.cpp:792` through `:801` | Separate left/right slate layers already exist: reuse is a smaller composition-focused experiment than creating a new compositor architecture. |
| Resolution coupling | `src/mods/vr/runtimes/OpenXR.cpp:381`; `D3D12Component.cpp:1234`, `:1600` | Game and swapchain sizes currently share HMD dimensions. Separating reduced scene size from presentation size is necessary for a full-FOV output resolve. |
| Culling setting | `src/mods/VR.cpp:1555`; `src/mods/VR.hpp:897` | “Disable Instance Culling” specifically forces `r.InstanceCulling.OcclusionCull=0`; this does not establish that all UE frustum culling is disabled. Do not change this compatibility default blindly. |

Epic documents stereo projection, view rectangles, and render-target sizing in [IStereoRendering](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/IStereoRendering) and [IStereoRenderTargetManager](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/IStereoRenderTargetManager). These current API descriptions support the design, but do not prove that every injected UE version uses these hooks for every visibility pass. No general-purpose portal-culling hook was identified in the inspected code.

## Composition versus physical perspective

The existing screen projection uses a hardcoded 90-degree horizontal FOV (`FFakeStereoRenderingHook.cpp:5134`). An aperture 2 meters wide at 2 meters distance subtends `2*atan(1/2)`, approximately 53.1 degrees at its center. Matching the screen's physical rectangle therefore does not automatically retain its full 90-degree game composition.

This is a geometric inference: a rigid or uniformly scaled mapping from tracking space to game space preserves angles. To fit the wider neutral-view picture into that smaller physical opening, deliberately choose a different virtual-camera calibration or nonuniform/projective mapping. That changes perceived world scale/depth and head-parallax response. Calibrate both eyes and translation together; changing FOV alone can produce inconsistent stereo. Label the result a composition-preserving calibration rather than claiming identical physical perspective. It may be exactly the user's preferred experience.

Consequently, a 90-degree composition rendered in a smaller screen may save many pixels but remove far less scene geometry than a 53-degree aperture. HUD fit is a separate matter from 3D frustum geometry.

## Smallest staged experiments

1. **Establish the intended image and baseline.** Reuse the existing screen's placement/aspect/HUD behavior for a full-picture target. Freeze a repeatable game-camera location, retain binocular disparity, and specify how head translation should change the view. Record CPU/GPU frame times at fixed quality and resolution settings.

2. **For the full-picture target, extend the stereo quad path first.** Compute a per-eye generalized off-axis view of the physical screen using a shared anchor and the chosen virtual-camera calibration. Feed engine view/projection hooks before SceneView construction. Preserve the existing per-eye game/HUD composition where possible; use a stable scene allocation sized to the screen's visible pixel demand. This can provide binocular depth and view-dependent parallax even though the runtime receives quads. OpenXR exposes eye visibility, physical size, pose, and space for a [quad layer](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrCompositionLayerQuad.html). The runtime sees a plane, however, and cannot infer interior scene depth for late translational reprojection. This is a limitation to test, not proof that this route is unacceptable.

3. **For the physical-aperture target, crop engine rays and scene pixels together.** Transform the aperture corners into each rendered eye's space. Clip against valid forward space, take conservative minimum/maximum `x/-z` and `y/-z`, intersect with the visible eye region, and add a guard band. An oblique aperture produces a quadrilateral, so a rectangular bounding frustum still needs the final exact aperture mask. Keep existing eye orientations initially. A portal-aligned camera can tighten the rectangle later, but changes additional view/submission transforms. Retain the game's normal near plane unless deliberately clipping everything in front of the portal is part of the design.

4. **Present cropped scene images correctly.** The compatibility-first option is a cheap resolve into the existing full-FOV output: fill black/green outside, sample the cropped scene only into its matching eye-ray rectangle, then apply the exact aperture mask. This retains current OpenXR FOV/pose submission and full-size final-image bandwidth while reducing upstream scene work. Alternatively submit a smaller projection image with matching rendered `pose`, `fov`, and `imageRect`. OpenXR permits different projection views/FOVs and maps them to the display; check [`fovMutable`](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrViewConfigurationProperties.html), runtime behavior, and the required surrounding-color layer before choosing this route. The source rectangle selects texels, while pose/FOV determine their angular mapping. See [projection views](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerProjectionView.html) and [swapchain subimages](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSwapchainSubImage.html).

5. **Verify culling, then optimize allocation.** Confirm that the engine actually constructs narrower visibility volumes, including any stereo-union/culling view. Reduce both scene view dimensions and relevant allocations so screen-space passes also do less work. Start with fixed or bucketed dimensions, not per-head-motion reallocations. A small viewport on a large allocation may reduce some passes but leaves allocation-wide work; a narrow projection at unchanged dimensions may merely spend the same shading budget on a smaller world sector.

## What each technique can save

These are predictions conditional on where the operation is integrated.

| Technique | Potential savings | Work it does not automatically remove |
| --- | --- | --- |
| Current final mask | None upstream; adds a small pass | Scene rendering, draw submission, simulation |
| Narrow engine frustum | Invisible-object draw/geometry work; fewer visibility/occlusion candidates | Fixed simulation, all offscreen shadow casters, reflection scenes, all shared stereo work |
| Smaller scene targets/view rectangles | Pixel shading, bandwidth, applicable screen-space passes | World simulation; geometry that remains visible |
| Scissor before scene draws | Raster/pixel work outside a rectangular bound | CPU visibility, submitted vertices, compute dispatches, passes resetting the scissor |
| Early depth/stencil aperture mask | Pixel work outside an irregular opening where early rejection applies | CPU/vertex work; compute effects; incompatible or later-only stencil tests |
| D3D12 VRS | Supported raster pixel-shader invocations | Geometry, full-resolution depth/stencil, compute lighting/post-processing without separate changes |

UE documents that view-frustum culling removes objects outside the view and provides `stat initviews` counters. Geometry savings depend on object bounds and scene structure; a large object intersecting the aperture still survives. [Epic visibility/culling documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine).

Scissor is [rasterizer state](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-rssetscissorrects); applying it at submission is too late. Stencil can avoid shader work only where testing happens early; [Microsoft's early-depth/stencil documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-attributes-earlydepthstencil) describes the relevant shader restrictions. A generic injector must identify affected scene passes and preserve their existing state, making both less simple than one late mask.

[Microsoft's VRS documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/vrs) distinguishes coarse pixel shading from full-resolution depth/stencil and requires Tier 2 for an image-controlled spatial rate. VRS has no zero-shading rate and is not a replacement for omitting unseen work. It is a secondary option after confirming that supported raster shading is the bottleneck.

## Risks and acceptance evidence

- Keep render-time eye poses, frusta, integer crop bounds, and aperture anchor together through submission. Updating only at Present cannot repair earlier culling or a mismatched view.
- Add enough overscan for pose error, temporal jitter, and image filters. More guard area reduces savings. The OpenXR subimage contract explicitly allows filtering to sample beyond the image rectangle; border initialization matters.
- Temporal AA/upscalers, exposure, motion vectors, bloom, screen-space reflections, and history invalidation need observation under changing projection/view dimensions. Do not assume all effects naturally tolerate moving crops.
- Quads reproject as planes. Projection images without correct depth also cannot guarantee correct late translational parallax; the current mask disables depth (`D3D12Component.cpp:556`). Future depth submission must match the composed image, crop and projection, including background/frame semantics. [OpenXR depth-layer contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerDepthInfoKHR.html).
- Test recenter, lean, head rotation/roll, approaching/crossing the plane, one-eye visibility, window outside the headset FOV, menus, HUD, and reference-space changes. A screen HUD may need remapping rather than being clipped by a physical aperture.
- Measure CPU game/render-thread and GPU times, scene dimensions, visible/frustum-culled primitives, and draw/triangle counts where available. Fixed resolution and a reproducible scene isolate the change from dynamic resolution and reprojection cadence. Capture normal stereo screen, unoptimized portal, and optimized equivalent portal separately.

For rough budgeting only, pixel-dependent time can scale approximately with rendered pixel fraction; fixed and geometry costs do not. If a hypothetical 10 ms GPU frame contains 6 ms of scalable pixel work and 4 ms of other work, rendering 30% as many scene pixels suggests `4 + 0.30*6 = 5.8 ms`, before added resolve/mask cost and before any geometry savings. This is arithmetic, not a prediction for Hogwarts Legacy or any headset. Only injected-game/headset measurements can establish the improvement and whether the geometry feels right.
