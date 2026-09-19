# Volumetric Frame prototype 03

Status: user confirmed prototype 03 works in FluidFlux on 2026-09-18.
Release x64 build, placement regression and nine D3D12 WARP mask cases passed.
Performance optimization is not implemented or benchmarked yet.

This experimental mode preserves UEVR's native stereo/6DoF game view through an
anchored 16:9 opening. It automatically aligns that opening with the captured game
HUD. It does not change the game's camera FOV or warp the world.

## Requirements and controls

- D3D12, OpenXR and **Native Stereo**.
- Disable **2D Screen Mode**, **Extreme Compatibility Mode**, and stereo emulation.
- Open UEVR's menu with Insert, then **VR > Runtime > Volumetric Frame (Experimental)**.
- Enable the frame and leave **Match Game UI (automatic)** on (the default).
  On activation, or when changing it from false to true, placement uses the
  normal UEVR stage-anchored UI screen pose from its standing origin, rotation
  offset and decoupled-pitch settings. It does not recenter to the instantaneous
  head pose. UI Size is height: the default 2 m height means about 3.56 m width
  at 16:9.
- **Move Game UI with Frame** is on by default. With it on, the game HUD shares
  the frame's anchored rectangle; a cylinder HUD temporarily becomes flat.
  With it off, the HUD keeps its normal pose, size, head-follow behavior and
  cylinder mode while the portal moves independently; it may extend outside the
  portal by design. Saved UI preferences are not overwritten.
- If **UI Follows View** is enabled, there is no fixed normal screen. Automatic
  matching therefore uses a stable stage UI placement so the portal does not
  depend on the current head direction; it does not change the saved setting.
- Turn automatic matching off to use the original manual width/distance controls.
  The game HUD still shares that manually sized frame when **Move Game UI with
  Frame** is enabled.
- **Recenter Frame** is explicitly head-based: it moves the frame, and the HUD
  when **Move Game UI with Frame** is enabled, in front of the current head.
- **Green Surroundings (0, 255, 0)** selects opaque green; unchecked selects black.
- Use UEVR's normal Save Config action to retain options. The spatial anchor is
  recreated each activation/session rather than persisted across tracking spaces.

UEVR's own settings panel is separate and remains usable. Unsupported rendering
settings leave ordinary rendering active. Feature defaults off. Use a 16:9 game
window for this first version: mapping an ultrawide HUD texture to the rectangle
does not cause the game to relayout its UI.

## FluidFlux headset acceptance

User-confirmed working target with our preceding build:
`C:/Dev/VR/UEVR/Game-demos/FluidFlux_3_0_1_Demo_UE532/FluidFlux.exe`.
The injector process entry is `FluidFlux-Win64-Shipping`.

1. Close FluidFlux and other injectors; connect Virtual Desktop as usual.
2. Launch FluidFlux, then inject with the separate prototype 03 package using
   OpenXR. Keep Native Stereo and a 16:9 game window.
3. Enable Volumetric Frame with automatic UI matching. Do not adjust frame width
   or distance. Check the game HUD and frame boundaries share one rectangle.
4. Turn your head, then toggle **Match Game UI** off and on. The frame (and linked
   HUD) should return to the fixed normal UI anchor, rather than the head pose.
5. Turn **Match Game UI** and **Move Game UI with Frame** off, then manually resize
   or change distance.
   The HUD pose, size, head-follow behavior and cylinder mode should remain
   unchanged while the portal moves independently.
6. Face a different direction and press **Recenter Frame**. This explicit action
   should use the current head pose; with Move Game UI on, the HUD follows it.
7. Test green surroundings with Virtual Desktop chroma-key passthrough, then
   disable the frame and check normal game/UI behavior returns.

Record any eye mismatch, boundary jitter, HUD clipping or interaction problems.
Automated shader checks do not replace this injected-game/headset acceptance.

## Scope and limitations

The D3D12 mask runs after scene rendering. This build does not reduce scene work;
performance optimization is a separate experiment. It uses submitted eye poses,
FOVs and integer crop rectangles, retaining game pixels inside the opening and
writing opaque black/green outside. When linked, the game HUD quad consumes the
same frame pose and dimensions. Crossing behind the one-sided plane hides the game.

Captured HUD placement is aligned; UI drawn directly in the 3D scene is not moved
by this change. Exact native desktop camera composition is not the goal. Optional
reference FOV by physically changing width/distance is specified as priority two.
Depth submission remains suppressed while the frame is active. Runtime late
reprojection and reference-space changes still need headset validation.
Multisampled swapchains are unsupported. Green game content may also be keyed out
by Virtual Desktop.

## Build and automated validation

Build Release x64 with the existing CMake setup. From a VS x64 developer prompt:

```bat
cl /nologo /EHsc /std:c++17 /Idependencies/submodules/glm tests/volumetric_frame_anchor.cpp /Febuild/frame-anchor-test.exe /Fobuild/frame-anchor-test.obj
build\frame-anchor-test.exe
cl /nologo /EHsc /std:c++17 /Idependencies/submodules/glm tests/volumetric_frame_d3d12.cpp /Febuild/frame-test.exe /Fobuild/frame-test.obj /link d3d12.lib dxgi.lib
build\frame-test.exe
python tests/volumetric_frame_crop_math.py
```

The placement-policy regression failed on prototype02's rematch decision and
passes after separating automatic matching from head-based recentering.
The WARP test executes the actual mask shader and compares nine cases pixel by
pixel, including shared HUD dimensions, offsets and rotated-frame geometry. The
six crop-math tests specify a future optimization; they do not exercise engine
culling or imply that optimization is present in this build.

See [the agreed spec](volumetric-frame-next-steps.md) and
[performance experiment](volumetric-frame-performance-experiment.md).
