# Quest 3 OpenXR integration

## Current milestone: OpenGL ES session and headset pose tracking

The Android app packages and initializes the Khronos OpenXR Android loader. On the emulation activity lifecycle, the OpenGL ES presentation path creates an Android OpenXR instance and session using its current EGL display/config/context. It creates LOCAL and VIEW reference spaces, processes session state events, and samples the predicted VIEW-space pose each active frame.

- Loader artifact: `org.khronos.openxr:openxr_loader_for_android:1.1.65`
- Android native ABI already includes `arm64-v8a`
- Loader initialization uses `XR_KHR_loader_init_android`
- Loader initialization status is written to the Azahar Android log
- `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` are paired in the OpenGL presentation loop
- `xrLocateSpace(VIEW, LOCAL, predictedDisplayTime)` supplies position (meters) and orientation (quaternion)
- Kotlin exposes `QuestOpenXr.headPose()` as `[x, y, z, qx, qy, qz, qw, valid]`

## Not implemented yet

This is a tracking milestone, not a complete VR renderer. It does not yet create eye swapchains, submit projection layers, render Azahar into both eye views, or integrate Touch controller actions. The OpenXR frame is currently ended with zero composition layers; the ordinary Android surface remains the visible output. The pose API is available to Kotlin, but is not yet consumed by the 3DS camera/player renderer.

## Next implementation stages

1. Add OpenXR stereo swapchains and projection-layer submission.
2. Route Azahar's OpenGL ES renderer into both eye targets with per-eye view/projection matrices.
3. Feed the tracked head pose into the VR camera/player transform.
4. Add controller action sets and map Quest Touch input to 3DS controls.
5. Add the immersive launcher intent only when stereo presentation is ready.
6. Build and test on physical Quest 3 hardware, retaining a normal Android fallback.

The app should not advertise itself as a fully immersive OpenXR title until the
session and stereo frame loop are working.
