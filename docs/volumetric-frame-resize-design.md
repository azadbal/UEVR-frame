# Coupled window and scene scale: design after prototype03

Source baseline: `c473285`, 2026-09-18. Prototype03 is user-confirmed working.
This is a proposed next behavior and mathematical specification, not an
implemented feature. Keep the native stereo 6DoF physical aperture; ignore
desktop FOV and retain the existing full-FOV runtime presentation.

## Proposed default

**Scale the window and the perceived scene together about the window center.**
A scale of 50% makes the physical width, height, and every perceived scene
distance measured from that center half their baseline values. Keep the
window center, orientation, and distance fixed while resizing. The aspect stays
16:9. This produces a smaller three-dimensional scene, with consistent stereo
and head-motion parallax, rather than simply covering more of the existing world.

Use one separate `Window and Scene Scale` control, initially 100%, applied to
the existing matched/manual base rectangle. Keep the current width/UI size as
the base aperture controls; those still set framing. Do not silently change the
saved global World Scale or saved UI size. Retain `Move Game UI with Frame`:
when on, its flat quad uses the scaled rectangle; when off, its normal size,
pose, head-follow and cylinder settings remain independent. UEVR's settings
panel remains independently usable. World-space UI belongs to the scene and
scales perceptually with it; a captured HUD is a separate compositor layer.

Distance stays separate, with its existing placement value, and is held fixed
during resizing. Validate fixed placement first. Existing distance remains an
aperture placement control; do not silently turn it into translation of the
entire scene. Changing the pivot while scaled needs its own continuity design
and validation. Coupled translation of the scene and window is outside this
resize proposal.

## What the current source actually does

| Source at baseline | Consequence |
| --- | --- |
| `src/mods/vr/D3D12Component.cpp:119` and `:135` | Anchor selection happens in the late mask path. Initial automatic matching uses the stable UI stage anchor; explicit recenter uses the current head center and yaw. |
| `src/mods/vr/D3D12Component.cpp:155` and `:170` | Width/height and frame pose are stored in shared layout, then used to draw the aperture. Changing size currently changes masking, not game camera/world scale. |
| `src/mods/vr/VolumetricFrameLayout.hpp:17` and `:34` | Anchor persistence and distance/X/Y offsets are explicit. The final pose translation, after offsets, is the window center; the anchor's origin alone is not the scale pivot. |
| `src/mods/vr/FFakeStereoRenderingHook.hpp:356` | `calculate_stereo_view_offset_` is a wrapper around the actual hook. Both call paths must agree. |
| `src/mods/vr/FFakeStereoRenderingHook.cpp:4899` | Camera forward/right/up offsets are added independently of World Scale. They belong in the baseline camera mapping. |
| `src/mods/vr/FFakeStereoRenderingHook.cpp:4907` and `:4939` | World Scale multiplies `world_to_meters`; that product multiplies both standing-origin-relative head translation and eye separation. Larger values make a fixed game scene appear smaller. |
| `src/mods/vr/FFakeStereoRenderingHook.cpp:4961` and `:5012` | Native head/eye rotation remains active; roomscale can move the pawn and update standing origin. A naive replacement of the local scale also changes roomscale gameplay. |
| `src/mods/VR.hpp:440` | The global scale also affects the general world-to-meters getter. |
| `src/mods/vr/IXRTrackingSystemHook.cpp:1138`, `:1220`, `:1270`; `src/mods/UObjectHook.cpp:681` | Controller/HMD reporting and object hooks consume scale too. A global settings change has effects beyond scene rendering. |
| `src/mods/vr/OverlayComponent.cpp:841` | Linked HUD already consumes the shared stage-space rectangle. |

## The transform and the required camera correction

Work in a consistent physical stage coordinate system, in meters. Let:

- `C`: fixed physical window center after all placement offsets.
- `E`: actual physical position of either tracked eye, including head motion.
- `O`: the standing/reference origin used by the current camera mapping.
- `A(t)`: current game camera position including UEVR's camera offsets.
- `k`: baseline game units per physical meter, including saved World Scale.
- `R(t)`: the baseline stage-to-game orthogonal coordinate mapping, including
  game rotation, recenter rotation and UEVR's axis/sign conventions.
- `s > 0`: coupled physical scale, with 1 meaning current behavior.

Abstract the baseline eye mapping as `V0(E) = A + k R(E - O)`. The source's
subtractions and quaternion conversions belong inside `R`; do not copy the
following vector formula into UE coordinates without that conversion.

For a game point `X`, let `P0(X)` be its baseline perceived stage position.
The intended presentation is:

```text
P_s(X) = C + s (P0(X) - C)
width_s = s width_0
height_s = s height_0
```

Render the unchanged game geometry from the inverse-transformed physical eye:

```text
Q(t)   = V0(C)                         # current game point at the portal center
V_s(E) = Q + (V0(E) - Q) / s
       = A + (k/s) R(E - O) + k (1 - 1/s) R(C - O)
```

The eye orientation and corresponding angular projection remain the baseline
ones. The actual OpenXR stage eye poses, IPD and runtime FOV are not modified.
Only the game render camera receives this inverse mapping. Its rays obey:

```text
X - V_s(E) = (k/s) R(P_s(X) - E)
```

Thus projected rays coincide with those of a physically scaled scene. This
requires inverse-scaling **both** virtual IPD and tracked head translation.
Scaling IPD alone changes stereo depth without matching translation parallax.

The existing World Scale convention supplies `k/s`, not `k*s`: for example,
50% perceived scale needs twice the baseline numerical multiplier. However,
**World Scale alone is insufficient**. It omits the last translation term and
scales about the standing-origin/game-camera mapping instead of the portal.
With an eye at the origin and a portal 2 m forward, halving scale needs the
virtual eye 2 baseline meters backward. Merely doubling World Scale leaves
the centered head at the original camera. A manual camera offset could imitate
one stationary case but will not reliably handle arbitrary portal offsets,
rotation, recentering and reference-origin changes.

Do not apply the operation incrementally to last frame's transformed camera.
Use that frame's unscaled baseline mapping and one absolute scale value, so
slider changes do not accumulate drift.

## Locomotion, anchors, and invariants

`Q(t)` must be derived from the current baseline game camera every frame; it is
not a permanently frozen coordinate in the game world. Normal game locomotion,
cutscenes and control rotation continue to update `A(t)` and `R(t)`. Do not
replace the game's camera with a fixed portal camera or cancel those updates.
For a pure game-camera movement `delta A`, the apparent stationary-world
movement is `-(s/k) R^-1 delta A`: gameplay moves the same game distance, seen
at the new scene scale. The physical portal remains stage anchored.

Roomscale is more delicate because the current hook moves the pawn using
`head_offset_flat` and then rebases the standing origin. Initially scope
validation to roomscale off. Before supporting it, separate the gameplay
movement bookkeeping from the render-camera scale and prove continuity across
rebases; do not silently multiply pawn movement by `1/s`. The same audit applies
to motion-controller/object attachments, aim ray origins and post-camera plugin
callbacks. They can otherwise disagree with the visible scene. These are
integration limitations, not reasons to implement only an IPD change.

Fixed during a resize: stage center/orientation/distance, aspect, physical eye
poses/IPD, head rotation, game state and saved base settings. Scaled: perceived
object dimensions, distances between scene points, scene depth about the portal,
aperture width/height and linked HUD dimensions. Relative proportions and
stereoscopic/translation consistency are preserved.

The portal plane stays fixed. Points that were on it stay on it, with their
in-plane coordinates scaled around its center. Points behind/in front remain
on the same side and their signed distances to that plane scale by `s`. This
is the relevant zero-parallax plane: a scene point there coincides with its
physical surface location in both eyes. It does **not** mean identical raw
pixel coordinates in the two full-FOV eye images. Do not introduce toe-in or
an arbitrary stereo convergence adjustment.

Recenter/reference-space changes must rebase the same shared transform for
camera, mask and linked HUD in one frame. The current renderer only creates
the final layout during swapchain masking, after engine views have rendered.
Coupled scale therefore needs a frame-associated layout prepared before view
construction and retained through submission. Reading the previous frame's
mutable layout in the camera hook is not sufficient.

## Differences from other controls

| Operation | What the viewer gets |
| --- | --- |
| Current aperture width only | Same world scale and camera rays; smaller width hides more of the world. |
| Coupled uniform scale at fixed distance | Smaller physical aperture and a miniature three-dimensional scene about its center, with consistent stereo and head translation. |
| Move the entire presentation nearer/farther | Same physical object/window sizes, different angular size and viewing geometry; it requires scene translation as well as aperture placement. Moving only the current mask does not move the scene. |
| Change game camera FOV or scale a finished stereo image | Changes projection/image magnification without implementing this physical 3D similarity; not the selected approach. |

Uniform 3D scaling at fixed eye-to-window distance is not an exact uniform
resize of every pixel in the original picture. For a portal at z=-2 m, a point
at (1,0,-4) moves to (0.5,0,-3) under 50% scale. Its horizontal ray slope becomes
2/3 of its previous value, while the centered portal's ray slope becomes 1/2.
Depth-dependent perspective changes are physically correct. Do not promise
identical edge composition or fitting the entire former picture at every depth.
Maintaining a chosen 2D framing is a different requirement. Scaling everything
about the eye instead would also move the portal distance, which is not the
proposed fixed-distance default.

## Sequence and acceptance

1. Keep the tested prototype03 package as the baseline. Review this resize
   contract before adding runtime controls; default coupled scale is 100%.
2. Run the standalone math specification. Prepare immutable shared layout
   early enough for game views; keep frame association through mask/HUD output.
3. Prototype a render-camera-only inverse scale plus portal-center correction
   and linked geometry, initially in the supported OpenXR/D3D12 native-stereo
   path with roomscale off and gamepad/stationary reference tests. Do not
   overwrite global World Scale, pawn movement, actor scales or world physics.
4. Verify 100% equals the accepted baseline; 50% and 200% keep the center and
   portal plane fixed; stereo depth and lateral/forward head parallax agree;
   game movement/turning remain live; linked and independent HUD modes behave
   as specified. Include off-center placement, head yaw/roll and recenter.
5. Audit near clipping, camera collision/culling, temporal history, plugins,
   controllers and roomscale before broad support. Keep depth submission off
   as prototype03 already does. The existing game near plane is not a portal
   clipping plane; very small scales can place the virtual camera far away and
   expose game-specific camera/culling limitations.
6. Apply the separate aperture rendering optimization to this transformed
   camera and shared physical aperture only after visual equivalence holds.
   Resizing itself does not guarantee fewer shaded scene pixels or faster FPS.

On 2026-09-18, all six tests passed with
`python tests/volumetric_frame_scale_math.py`: 2,000 randomized inverse
mapping/ray identities plus portal-pivot correction, IPD/head translation,
plane/aspect invariants, live game locomotion and perspective behavior. It does
not test production hooks or headset comfort and makes no runtime performance
claim. Existing crop-math tests remain a separate optimization specification.
