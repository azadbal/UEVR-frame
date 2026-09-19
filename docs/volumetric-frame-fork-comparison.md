# Volumetric frame comparison: UEVR-6DOF-Window

Research compares the local `feature/volumetric-frame` commit
`c47328530b538d39bed14e9e5963c9295ff031a3` with the upstream fork branch
commit `fb31341e860b15e116a15123820c95f044ff0a0f` (`agent/6dof-window-mode`).
The fork branch is based on UEVR commit
`74b76bc9428a906cbdc69de3ebc1905fd0e9cc57`.

## Feature comparison

| Area | UEVR-6DOF-Window fork | Local `c473285` frame | Finding |
|---|---|---|---|
| Rendering scope | `WindowMode` draws a final color mask for D3D11/D3D12, OpenXR/OpenVR, and the three UEVR rendering methods. The README states that UI and depth swapchains are not modified. | The mask is implemented in the D3D12/OpenXR native-stereo path and is unsupported for AFR, 2D, extreme compatibility, stereo emulation, and non-D3D12 backends. Depth submission is disabled while active. | The fork has broader backend coverage; the local path has deliberate constraints and a shared game-HUD presentation path. |
| Geometry controls/defaults | `WindowMode.hpp` defaults to enabled=false, width=2.4 m, height=1.35 m, distance=2.0 m, feather=0.10 m, square corners, flat curvature, black surround, opacity=1.0. The menu exposes exact width/height, an optional 16:9 lock, distance, feather, corner radius, curvature, RGB surround color, and opacity. | The frame defaults to enabled=false, automatic UI matching=true, move-game-UI=true, manual width=2.0 m, distance=2.0 m, and black/green surround. Matching derives a 16:9 rectangle from the existing Slate `UI_Size`; manual mode remains fixed 16:9. | Feather, shape, curvature, and opacity are reusable QoL ideas. The fork's fixed physical width/height controls are more general than the local fixed aspect. |
| Anchor/recenter | Captures HMD position and basis on first valid frame or explicit recenter, then keeps that room-space anchor while the head moves. The menu reports `anchored` or `waiting for tracking`. | Automatic matching anchors from the normal stage UI placement, independently of the instantaneous head pose. Explicit **Recenter Frame** uses current head position/yaw. | The local behavior already separates automatic UI matching from explicit head recentering. The fork's visible anchor status and first-valid-frame handling are useful additions. |
| Game HUD/UI | The fork explicitly leaves UI swapchains alone; no game-HUD matching or coupled HUD resize is present in `WindowMode`. | `VolumetricFrameLayout` is shared with the OpenXR game Slate quad when **Move Game UI with Frame** is enabled; disabling it leaves the HUD pose, size, head-follow behavior, and cylinder mode independent. | The local implementation already covers the requested automatic alignment and independent-HUD toggle more directly than the fork. |
| Background | Configurable surround RGB and opacity. The shader applies alpha-blended surround color outside the aperture. | A boolean selects black or green surroundings; no arbitrary color or opacity control. | RGB/opacity is a small, user-visible extension if desired. |
| Resize/grab interaction | Sliders (with Ctrl+click exact entry) and a recenter button in the in-game menu. No in-world pointer grab, drag-to-move, controller resize, or WindowMode-specific shortcut is present. | Sliders and a recenter button in the existing menu; no in-world grab/resize. | No interactive resize implementation to port from the fork. |
| Keyboard/controller | The README documents Insert or XInput L3+R3 for opening the global menu and RT camera shortcuts. These are existing UEVR menu controls, not WindowMode-specific bindings. | Uses the same existing menu and an explicit frame recenter button; no dedicated frame shortcut. | No dedicated fork input behavior to reuse. |
| World scale | The new `WindowMode` code uses physical meter settings directly in tracking-space geometry. It has no reference to UEVR's `get_world_scale()` or a coupled world/window scale control. | Manual frame dimensions are also physical tracking-space values; automatic mode derives dimensions from Slate `UI_Size`. Existing global world-scale support is separate. | The fork does not solve coupled world/window scaling. Treat any such coupling as new design work. |
| Persistence | All WindowMode values are registered in `m_options`; generic `Mod::on_config_load/save` persists their `WindowMode_*` names in each game's normal `config.txt`. The feature is off by default. | Frame options are registered in `VR::m_options`, so the existing per-game config machinery persists them. The local automatic-match, move-HUD, width, distance, and surround-mode choices are saved. | Both have per-game persistence. The fork's transient CutsceneComfort bridge intentionally does not overwrite normal WindowMode settings. |

## Rendering details worth learning from

Backend implementations are not equivalent to validated runtime coverage.
The fork's release notes primarily document D3D11/OpenXR validation; D3D12
loads and initializes, but trustworthy final submitted-eye visual comparison
remains open. This comparison did not run the fork in a headset.

The fork's final-color injection is a separate `WindowMode` module with a
shared D3D11/D3D12 shader model. It handles flat and cylindrical ray
intersection, rounded-rectangle signed distance, and feathering in the shader;
it also backs up and restores D3D11 pipeline state around its draw. Its D3D11
RTV cache is bounded at 16 entries, and the D3D12 path transitions the target
to render-target state and back. These are concrete rendering patterns, but
they are a larger backend expansion than the local feature's current scope.

The fork's release notes call out a persistent D3D12 RTV per OpenXR
color-swapchain image to avoid descriptor aliasing while command lists are in
flight. The local D3D12 path already creates a target RTV for the frame mask;
the fork's per-image lifetime rule is the useful part to review if swapchain
recreation or in-flight command lifetime becomes a bug.

The local implementation has an important property the fork does not: it
builds one transient `VolumetricFrameLayout` during the mask pass and consumes
that same pose and size while generating the game Slate quad in the same
submitted frame. That is why the automatic-match and independent-HUD toggle
can stay coherent with the mask.

## Ranked reusable ideas

1. **High value / low-to-medium effort: visible anchor status and explicit
   first-valid-frame handling.** Keep the local automatic UI anchor and
   explicit head recenter semantics, then expose a small `anchored` /
   `waiting for tracking` status and consume recenter requests once per valid
   frame. This is a QoL improvement, not a code port.
2. **Medium value / medium effort: optional edge polish.** Consider feather
   width and corner radius while retaining the matched 16:9 rectangle. Keep
   these opt-in: rounded corners can hide corner HUD elements and feathering
   can interfere with green-screen passthrough. Independent aspect ratios and
   curvature need later design, especially for matching a curved game HUD.
3. **Medium value / low effort: configurable surround color and opacity.**
   Generalize the current black/green switch to RGB plus alpha while keeping a
   simple black default and the exact opaque RGB(0,255,0) passthrough preset.
   This is isolated from HUD pose and anchoring.
4. **Maintenance reference: per-swapchain RTV lifetime discipline.**
   Review the fork's persistent D3D12 RTV-per-image pattern if the local path
   sees command-list-in-flight or swapchain-recreation issues. Validate it
   against the existing `TextureContext` ownership before changing it.
5. **Medium value / medium effort: transient cutscene override contract.**
   A versioned custom event can apply temporary aperture settings without
   writing the user's normal per-game frame profile. This is useful only if a
   separate cutscene plugin needs that contract; it is unrelated to ordinary
   game-HUD matching.

## Licensing and provenance

At the pinned fork commit, `LICENSE` contains only:

> Copyright (c) 2022-2025 praydog
>
> All rights reserved.

The local UEVR `LICENSE` has the same text. Neither file grants a standard
permissive reuse license. This is a factual provenance note, not legal advice:
literal source-code porting or cherry-picking from the fork should wait for a
clear permission or license grant. The recommendations above are behavioral
ideas grounded in the inspected code; they do not recommend copying the
fork's implementation.

## Source URLs and verification boundary

- [Fork branch at pinned commit](https://github.com/elliotttate/UEVR-6DOF-Window/tree/fb31341e860b15e116a15123820c95f044ff0a0f)
- [Fork README](https://github.com/elliotttate/UEVR-6DOF-Window/blob/fb31341e860b15e116a15123820c95f044ff0a0f/README.md)
- [Fork WindowMode.hpp](https://github.com/elliotttate/UEVR-6DOF-Window/blob/fb31341e860b15e116a15123820c95f044ff0a0f/src/mods/WindowMode.hpp)
- [Fork WindowMode.cpp](https://github.com/elliotttate/UEVR-6DOF-Window/blob/fb31341e860b15e116a15123820c95f044ff0a0f/src/mods/WindowMode.cpp)
- [Fork D3D11 integration](https://github.com/elliotttate/UEVR-6DOF-Window/blob/fb31341e860b15e116a15123820c95f044ff0a0f/src/mods/vr/D3D11Component.cpp)
- [Fork D3D12 integration](https://github.com/elliotttate/UEVR-6DOF-Window/blob/fb31341e860b15e116a15123820c95f044ff0a0f/src/mods/vr/D3D12Component.cpp)
- [Fork release notes](https://github.com/elliotttate/UEVR-6DOF-Window/blob/fb31341e860b15e116a15123820c95f044ff0a0f/RELEASE_NOTES_v0.2.0-alpha.1.md)
- [Fork LICENSE](https://github.com/elliotttate/UEVR-6DOF-Window/blob/fb31341e860b15e116a15123820c95f044ff0a0f/LICENSE)
- [Local comparison commit](https://github.com/azadbal/UEVR-frame/tree/c47328530b538d39bed14e9e5963c9295ff031a3)
- [Local frame layout](https://github.com/azadbal/UEVR-frame/blob/c47328530b538d39bed14e9e5963c9295ff031a3/src/mods/vr/VolumetricFrameLayout.hpp)
- [Local D3D12 mask path](https://github.com/azadbal/UEVR-frame/blob/c47328530b538d39bed14e9e5963c9295ff031a3/src/mods/vr/D3D12Component.cpp)
- [Local game-HUD quad path](https://github.com/azadbal/UEVR-frame/blob/c47328530b538d39bed14e9e5963c9295ff031a3/src/mods/vr/OverlayComponent.cpp)

Not verified here: visual behavior of the fork's D3D12 submitted-eye path, any
runtime behavior outside the README/release-note validation claims, and any
permission beyond the text of the two checked-in LICENSE files.
