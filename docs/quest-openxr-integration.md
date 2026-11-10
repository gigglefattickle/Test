# Quest 3 OpenXR integration

## Current milestone: world-locked immersive game screen

The Android app initializes the Khronos OpenXR Android loader and creates an Android/OpenGL ES OpenXR session from the emulator's EGL context. The OpenGL presentation loop now:

- Processes OpenXR session state transitions and pairs `xrWaitFrame`, `xrBeginFrame`, and `xrEndFrame`.
- Locates the headset pose in LOCAL space and exposes it to Kotlin as `QuestOpenXr.headPose()`.
- Creates a GL_RGBA8 OpenXR swapchain.
- Copies the rendered Azahar game screen into an acquired swapchain image.
- Submits that image as an OpenXR quad composition layer positioned 2 metres in front of the LOCAL origin. The Quest compositor therefore keeps the game screen stable in the room as the player moves their head.
- Registers the emulation activity with the Meta/Khronos immersive-HMD intent category and declares 6DoF head-tracking support.

## What this milestone does not do

This is a working immersive-screen path, not yet a native stereoscopic VR conversion of the game world. Both eyes see the same flat game image on a world-locked virtual screen. The 3DS camera/player renderer does not yet consume the head pose, and Quest Touch action sets are not implemented. It also has not yet been compiled into an APK or tested on physical Quest 3 hardware.

## Next implementation stages

1. Build the Android arm64 APK and fix any NDK/OpenXR integration errors.
2. Test session startup, pause/resume, swapchain presentation, and shutdown on Quest 3.
3. Add per-eye rendering and integrate the tracked head pose into the game camera/player transform where the game renderer supports it.
4. Add OpenXR action sets and map Quest Touch controllers to 3DS controls.
5. Tune screen position, scale, and resolution from in-headset testing.

Do not describe this milestone as full stereoscopic VR until per-eye game rendering and tracked camera integration are complete.
