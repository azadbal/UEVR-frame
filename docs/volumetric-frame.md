# Volumetric Frame

An anchored 16:9 portal into UEVR's stereo, six-degree-of-freedom game world.
Automatic placement matches the game HUD; leaning changes the view through the
opening. Desktop camera composition is not calibrated or reproduced exactly.

Prototype 12's projection crop with Native Stereo Fix ON was confirmed working
well by the user in FluidFlux on 2026-09-20. Prototype 13 polishes controls and
developer tooling; it does not change the renderer. Hogwarts validation of the
new native-fix crop path is still pending.

## Controls

Open **VR > Runtime > Volumetric Frame (Experimental)** in UEVR's injected menu.
The feature requires D3D12, OpenXR and Native Stereo. Disable 2D Screen Mode,
Extreme Compatibility Mode and stereo emulation. Native Stereo Fix can stay ON.

- **Enable Volumetric Frame:** enables the anchored opening. Defaults off.
- **Match Game UI (automatic):** uses the normal stage-anchored UI position,
  independently of where you are looking when you enable it. UI Size is height:
  2 m means about 3.56 m wide at 16:9. Head-following UI preferences are preserved,
  but the matching anchor stays fixed. Disable matching for manual width/distance.
- **Move Game UI with Frame:** links the captured HUD to the portal. Disable it
  to preserve the HUD's normal pose, size and head-follow behavior. Linked curved
  HUDs become flat; saved UI preferences are not overwritten.
- **Recenter Frame:** explicitly places the portal in front of your current head
  position and direction. The linked HUD follows.
- **Green Surroundings (0, 255, 0):** selects chroma-key green; otherwise black.
- **Projection Crop (Experimental):** concentrates full-resolution scene rendering
  into the portal. Requires SceneView and SplitScreen compatibility OFF.

Hover the `(?)` markers for explanation. Experimental rendering and diagnostics
are in collapsed sections; collapsing them does not change saved settings. Use
UEVR's normal Save Config action to retain preferences. The anchor is recreated
each activation/session. Keep the game's window at 16:9: stretching an ultrawide
HUD texture into this opening does not relayout the game's UI.

## Sharpness and performance

Crop-only keeps full scene view dimensions while narrowing each eye's projection.
More samples cover each visible detail, which can improve sharpness. Unreal may
also skip objects outside that view, but pixel shading is not automatically
cheaper and image reconstruction adds work. A speedup is not guaranteed.

For more rendered detail, use **OpenXR Options > Resolution Scale**. It scales
both dimensions: increasing 1.0 to 1.1 requests about 21% more pixels, before
other engine/runtime resolution behavior. Check the displayed render dimensions
and frame times. Extra samples do not create detail absent from game assets.
An independent portal quality control would require separate source/output sizing;
it is not implemented by this polish pass.

**Reduce Scene Pixels** remains an opt-in experiment under Experimental Rendering.
It reduces active scene view dimensions, trading some crop-only supersampling for
less pixel work. Full texture allocations remain. It is blocked with Native Stereo
Fix ON. **Fixed View Scale** is a diagnostic variant, not a quality preset: it
changes horizontal/vertical sampling and may not match the live crop's quality.

The reduction idea remains valid, but compatibility and net cost are unresolved.
FluidFlux showed shorter engine intervals under high pixel load; Deep Rock showed
regressions. Neither proves a single implementation bug or universal benefit.
Future work should first freeze each eye's reference crop dimensions, compare
equivalent sampling during controlled motion, then measure Unreal scene GPU time,
temporal stability and total frame time. Native-fix pixel reduction is separate
work. Keep historical evidence in prototypes 10/11; removal remains deferred.

## Diagnostics and validation

**Diagnostics > Performance Diagnostics** saves settings, applied paths, recovery
and scoped UEVR GPU/CPU timings. It does not measure whole-game GPU time. Logging
is opt-in and synchronous, so disable it for normal play. Per-game `log.txt` is
archived into a unique `logs/` file at the next injection.

**Show Engine Statistics** uses Unreal's `stat unit`; **Show FPS** uses `stat fps`.
The automated runner can enable the overlay but does not export its numeric values.

Use `tests/run-frame-benchmark.ps1` for launch/injection, warmed comparisons,
optional stereo captures, log archival and profile restoration. Native-fix runs
accept baseline/crop only. The old prototype-07 smoke helper is historical.
The unreliable `native_scaled` launch mode was retired; historical analysis stays
readable. Run `python -m unittest discover -s tests -p "test_*frame*.py"` for the
analysis regressions; native geometry and D3D12 test build commands are in their
source headers. Keep these regressions and frame-association safeguards.

For headset checks, compare crop OFF/ON/OFF at a fixed viewpoint, then turn/lean,
check both eyes, recenter, HUD alignment, green/black and loading recovery.
Headset appearance, engine tick intervals and GPU timings are separate evidence.
See [prototype 12](volumetric-frame-prototype-12.md) for native-fix implementation
and the recorded manual test.

Depth submission stays disabled while the frame is active. Crossing behind the
one-sided portal hides the game. Multisampled swapchains are unsupported. HUD
drawn directly in the world is not repositioned; green game content can be keyed
out by Virtual Desktop. Coupled portal/world resizing and reference-FOV controls
remain deferred.
