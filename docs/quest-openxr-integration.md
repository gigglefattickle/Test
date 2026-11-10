# Quest 3 OpenXR integration

## Current milestone: stereoscopic 3DS eye presentation

The Android app initializes the Khronos OpenXR Android loader and creates an Android/OpenGL ES OpenXR session from the emulator EGL context. The frame loop now obtains the two primary stereo view poses/FOVs from the Quest runtime and submits an OpenXR projection layer backed by a two-layer texture-array swapchain.

The OpenGL renderer exposes the 3DS top-screen left- and right-eye framebuffer textures. Each eye is copied independently into its matching OpenXR swapchain array layer, and each projection view uses the matching Quest eye pose and FOV. This preserves actual 3DS stereoscopic depth when a title supplies separate right-eye framebuffers. If a title has no active right-eye buffers, the left-eye image is mirrored to both eyes so the game remains visible, but that fallback has no stereoscopic depth.

- Loader artifact: `org.khronos.openxr:openxr_loader_for_android:1.1.65`
- OpenXR primary stereo view configuration with two runtime-recommended eye dimensions
- Array swapchain with two image layers
- Per-eye image copy from the native 3DS top-screen framebuffer
- Per-eye `XrView.pose` and `XrView.fov` submitted through `XR_TYPE_COMPOSITION_LAYER_PROJECTION`
- Existing head-pose API remains available to Kotlin
- Immersive Quest activity categories and 6DoF head-tracking declaration remain enabled

## Remaining work

1. Build the Android arm64 APK and fix any compile/link issues.
2. Test stereo separation, eye order, crop/orientation, and runtime stability on Quest 3 hardware.
3. Verify that a supported 3DS title is producing distinct left/right framebuffer data; enable the emulator's 3D output where required.
4. Add controller action sets and map Quest Touch controllers to 3DS controls.
5. Integrate head movement into a true room-scale game camera/player transform. The current projection layer uses the Quest's tracked per-eye views, but the 3DS world itself is still rendered by its original game camera.

This is stereoscopic presentation of the 3DS game's own left/right images. It is not yet a fully room-scale, six-degree-of-freedom conversion of the original game world.


## Quest Touch input and room-anchored presentation

The Android OpenXR session now creates an action set for both Quest Touch controllers using the Oculus Touch interaction profile. It samples face buttons, trigger/grip clicks, thumbstick clicks, analog trigger/grip values, thumbsticks, grip poses, and aim poses. Android forwards the mapped 3DS buttons and circle-pad/C-stick values into Azahar's existing gamepad input path.

The tracked aim poses are also projected into each eye's view. A colored laser beam and reticle are composited over the stereo output and intersect the virtual game panel at a fixed local-space plane.

The game image is now presented on a 5:3 panel anchored in OpenXR LOCAL space rather than filling the headset view. The panel is projected separately for each eye from the runtime's predicted eye pose and FOV. Head translation and rotation therefore change the panel's position and apparent size naturally as the player moves around it.

**Important limitation:** this is room-anchored 6DoF presentation of the emulator's existing left/right 2D framebuffer images, not full 6DoF movement inside the Animal Crossing world. The current emulator handoff does not expose the game's world geometry, depth buffer, or a controllable in-game VR camera. True room-scale world navigation requires a deeper renderer/game-camera integration (or depth-aware scene reconstruction); the flat framebuffer cannot reveal surfaces hidden from the original game camera.
