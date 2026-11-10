// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version.

#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <jni.h>

extern "C" JNIEXPORT jint JNICALL
Java_org_citra_citra_emu_QuestOpenXr_initializeLoader(JNIEnv* env, jobject /* thiz */,
                                                       jobject application_context) {
    JavaVM* application_vm = nullptr;
    if (env->GetJavaVM(&application_vm) != JNI_OK || application_vm == nullptr ||
        application_context == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    const XrResult proc_result = xrGetInstanceProcAddr(
        XR_NULL_HANDLE, "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
    if (XR_FAILED(proc_result) || initialize_loader == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader_info.applicationVM = application_vm;
    loader_info.applicationContext = application_context;

    return initialize_loader(
        reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info));
}
