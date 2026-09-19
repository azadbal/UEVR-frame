# 2D screen mode and portal framing research

Historical investigation. The user subsequently chose to preserve the existing
physical-aperture rendering and ignore desktop FOV. See
[the agreed spec](volumetric-frame-next-steps.md); full-picture camera-calibration
recommendations below are not the selected implementation.

This note is source investigation for a portal that should resemble the physical
16:9 screen used by UEVR's 2D mode while retaining stereo and tracked 6DoF.
The source citations are local `path:line` references. “Finding” means directly
visible in source; “inference” is derived from those findings.

## Existing 2D mode

- **Finding — the toggle is a persistent VR option.** `2DScreenMode` is a
  `ModToggle`, and the runtime key toggles the same object
  (`src/mods/VR.hpp:904`, `src/mods/VR.cpp:1951-1953`). The mode is also exposed
  in the Runtime page (`src/mods/VR.cpp:2399-2404`).
- **Finding — its “screen size” is render-target pixels, not a physical
  monitor.** In 2D mode `get_hmd_width/height()` return the framework render
  target size (multiplied by OpenXR resolution scale), rather than a headset
  recommended size (`src/mods/VR.cpp:2258-2287`). `get_rt_size()` is the last
  D3D backbuffer size; it defaults to 1920x1080 and is updated from the actual
  backbuffer descriptor (`src/Framework.hpp:188-202`, `src/Framework.hpp:296`,
  `src/Framework.cpp:1865-1868`, `src/Framework.cpp:2005-2010`). There is no
  monitor physical-width or viewing-distance query in this path.
- **Finding — D3D12 builds screen textures at the framework RT size.** Two
  `m_2d_screen_tex` resources use `get_d3d12_rt_size()` for width and height
  (`src/mods/vr/D3D12Component.cpp:1320-1342`). The OpenXR UI, UI_RIGHT, and
  FRAMEWORK_UI swapchains likewise use that desktop RT size
  (`src/mods/vr/D3D12Component.cpp:1655-1679`). `UI_RIGHT` is explicitly the
  second eye for stereoscopic 2D (`src/mods/vr/runtimes/OpenXR.hpp:324-338`).
- **Finding — 2D composes a full picture.** The D3D12 path copies the left half
  of the combined game texture to the left screen texture and the right half
  (or scene-capture texture) to the right texture. It then composites
  `m_game_ui_tex` into each eye and clears the original scene targets
  (`src/mods/vr/D3D12Component.cpp:421-490`). Thus the game HUD pixels are part
  of the screen image when available.
- **Finding — UEVR GUI remains a separate layer.** In OpenXR 2D mode the two
  screen textures become separate Slate quad layers, while the framework RT is
  copied to `FRAMEWORK_UI` and generated as its own quad
  (`src/mods/vr/D3D12Component.cpp:520-539`, `src/mods/vr/D3D12Component.cpp:792-814`).
  “Game HUD” and “UEVR GUI” therefore have different placement paths.

## Existing camera and pose behavior

- **Finding — 2D mode suppresses tracked head pose.** During stereo view-offset
  calculation, 2D mode skips `head_offset` (translation) and skips writing the
  HMD-derived Euler rotation. It still subtracts per-eye separation, so the
  mode keeps an eye pair but not tracked head translation/orientation
  (`src/mods/vr/FFakeStereoRenderingHook.cpp:4919-4958`). Stereo emulation can
  suppress all headset transformations (`src/mods/vr/FFakeStereoRenderingHook.cpp:4919-4923`).
  **Inference:** enabling this mode directly cannot satisfy a portal requirement
  for stereo plus 6DoF.
- **Finding — its projection is a hardcoded 90-degree horizontal FOV.** The
  2D projection branch uses `fov = 90.0f`, derives aspect from
  `get_hmd_width/height()`, and writes a symmetric projection matrix. The source
  leaves a TODO to obtain FOV from `FMinimalViewInfo`
  (`src/mods/vr/FFakeStereoRenderingHook.cpp:5134-5161`). It does not preserve
  the game's original camera FOV, off-axis projection, or camera framing.
- **Inference — physical plane geometry and image framing are separate
  contracts.** A 2 m-wide portal at 2 m has a horizontal angular span of
  `2*atan(1/2) = 53.13°`, while the existing 2D camera renders 90° horizontally.
  Reusing a physical pose/size alone therefore does not guarantee that the
  original 2D composition fills the portal; it can crop or leave unused image
  area. Matching a 90° camera with geometrically faithful aperture rays would
  require that angular span at the reference eye position. Fitting that picture
  into a smaller physical screen is possible with explicit camera calibration,
  but changes the relationship between physical movement and scene perspective.

## Existing Slate placement and reusable pieces

- **Finding — the OpenXR Slate quad already has stage/view-space placement.**
  With `UI_FollowView` enabled it uses `view_space`; otherwise it builds a
  stage-space matrix from inverse rotation offset plus standing origin, then
  applies distance and X/Y offsets (`src/mods/vr/OverlayComponent.cpp:839-870`).
  This is useful as a reference for a world-anchored portal pose, but it is an
  overlay pose and does not provide physical monitor dimensions.
- **Finding — Slate dimensions are aspect-derived meters.** The quad uses
  `UI_Size` as its height and computes width as swapchain aspect times that
  height (`src/mods/vr/OverlayComponent.cpp:859-862`). Defaults are distance 2 m,
  size/height 2 m, and zero X/Y offset (`src/mods/vr/OverlayComponent.hpp:102-108`),
  so at 16:9 the default Slate quad is approximately 3.56 m wide by 2 m high.
  This differs from the volumetric prototype's separate 2 m-wide frame defaults
  (`docs/volumetric-frame.md:3-5`).
- **Reusable:** the standing-origin/rotation-offset convention and the
  aspect-to-meter calculation are good anchoring references; the 2D copy path
  can supply a scene-plus-game-HUD image when a full-picture portal is desired.
- **Not sufficient:** `get_hmd_width/height()` and the Slate quad do not identify
  a real monitor's physical size or guarantee camera framing. The Slate quad is
  an alpha-blended compositor layer, while a volumetric portal also needs its
  own scene mask/background and normal stereo view path.

## Recommendations and open decisions

1. Define portal geometry explicitly as an aspect (hardcode 16:9 for now), a
   physical width/height, and a distance. Do not infer physical meters from RT
  pixels. Reuse `UI_Size` as physical height (its existing documented code
  meaning), plus screen pose/distance, to match the user's 2D screen. Make later
  aspect customization a single geometry parameter.
2. Preserve native-stereo tracking inputs, but derive portal-specific per-eye
   views/projections. Merely keeping the normal headset projection and stretching
   its image onto a screen does not reproduce the intended portal perspective.
   Reuse the 2D presentation code separately from `is_using_2d_screen()`, whose
   view-offset branch deliberately removes tracked head pose.
3. Choose composition semantics explicitly. The recommended initial behavior
   is the existing 2D “full picture” concept: scene plus game UI in the portal,
   with UEVR framework UI remaining separately controlled. Placement-only
   masking cannot promise that the game HUD matches the screen edges.
4. Treat 90° HFOV versus physical 16:9 geometry as an unresolved product choice:
   matching the existing 2D camera, matching a chosen physical screen, and
   preserving each game's original camera framing are different behaviors.

The main ambiguity is the word “HUD”: source code clearly separates game HUD
(`m_game_ui_tex`, composited into 2D screen textures) from UEVR's framework UI
(`FRAMEWORK_UI`). A final portal design should specify which one is meant and
whether the game camera should retain its original FOV or emulate 2D mode's
90° projection.
