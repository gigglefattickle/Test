# Quest 3 OpenXR integration

## Current milestone: Android loader bootstrap

The Android app now packages the Khronos OpenXR Android loader and initializes
it from the application context at startup. The native Android target links
against the Prefab-provided `OpenXR::openxr_loader` target.

- Loader artifact: `org.khronos.openxr:openxr_loader_for_android:1.1.65`
- Android native ABI already includes `arm64-v8a`
- Loader initialization uses `XR_KHR_loader_init_android`
- Loader initialization status is written to the Azahar Android log

## Not implemented yet

This change does **not** create an OpenXR instance or session. It does not yet
acquire headset poses, create eye swapchains, submit stereo frames, or route
Azahar's renderer into OpenXR. It is the loader/bootstrap layer only.

## Next implementation stages

1. Add a dedicated immersive Android activity and the Meta/Khronos immersive
   intent category once the activity actually owns an OpenXR session.
2. Implement instance/system/session lifecycle with
   `XR_KHR_android_create_instance`.
3. Create Vulkan or OpenGL ES graphics bindings and per-eye swapchains.
4. Connect Azahar's existing renderer to the OpenXR eye targets.
5. Add controller action sets and map Quest Touch input to 3DS controls.
6. Test on physical Quest 3 hardware and retain a normal Android fallback.

The app should not advertise itself as a fully immersive OpenXR title until the
session and stereo frame loop are working.
