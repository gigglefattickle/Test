// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version.

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <android/log.h>
#include <jni.h>

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "quest_openxr.h"

namespace {
std::mutex g_mutex;
JavaVM* g_vm{};
jobject g_activity{};
bool g_resumed{};
XrInstance g_instance{XR_NULL_HANDLE};
XrSystemId g_system{XR_NULL_SYSTEM_ID};
XrSession g_session{XR_NULL_HANDLE};
XrSpace g_local_space{XR_NULL_HANDLE};
XrSpace g_view_space{XR_NULL_HANDLE};
bool g_session_running{};
bool g_frame_begun{};
XrTime g_display_time{};

void Log(const char* message) {
    __android_log_write(ANDROID_LOG_INFO, "AzaharOpenXR", message);
}

bool HasExtension(const char* wanted) {
    uint32_t count{};
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) return false;
    std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data()))) return false;
    for (const auto& property : properties) {
        if (std::strcmp(property.extensionName, wanted) == 0) return true;
    }
    return false;
}

void DestroySessionLocked() {
    if (g_frame_begun && g_session != XR_NULL_HANDLE) {
        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime = g_display_time;
        end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xrEndFrame(g_session, &end_info);
        g_frame_begun = false;
    }
    if (g_session_running && g_session != XR_NULL_HANDLE) {
        xrEndSession(g_session);
        g_session_running = false;
    }
    if (g_view_space != XR_NULL_HANDLE) xrDestroySpace(g_view_space);
    if (g_local_space != XR_NULL_HANDLE) xrDestroySpace(g_local_space);
    if (g_session != XR_NULL_HANDLE) xrDestroySession(g_session);
    if (g_instance != XR_NULL_HANDLE) xrDestroyInstance(g_instance);
    g_view_space = XR_NULL_HANDLE;
    g_local_space = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_instance = XR_NULL_HANDLE;
    g_system = XR_NULL_SYSTEM_ID;
}

void PollEventsLocked() {
    if (g_instance == XR_NULL_HANDLE) return;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (changed->session != g_session) {
                event = {XR_TYPE_EVENT_DATA_BUFFER};
                continue;
            }
            if (changed->state == XR_SESSION_STATE_READY && g_resumed && !g_session_running) {
                XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &begin_info))) {
                    g_session_running = true;
                    Log("OpenXR session started");
                }
            } else if (changed->state == XR_SESSION_STATE_STOPPING && g_session_running) {
                xrEndSession(g_session);
                g_session_running = false;
            } else if (changed->state == XR_SESSION_STATE_EXITING ||
                       changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                g_session_running = false;
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}
} // namespace

extern "C" JNIEXPORT jint JNICALL
Java_org_citra_citra_emu_QuestOpenXr_initializeLoader(JNIEnv* env, jobject,
                                                       jobject application_context) {
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || vm == nullptr || application_context == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    const XrResult proc_result = xrGetInstanceProcAddr(
        XR_NULL_HANDLE, "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
    if (XR_FAILED(proc_result) || initialize_loader == nullptr) return XR_ERROR_INITIALIZATION_FAILED;

    XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader_info.applicationVM = vm;
    loader_info.applicationContext = application_context;
    const XrResult result = initialize_loader(
        reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info));
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_mutex);
        g_vm = vm;
    }
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_org_citra_citra_emu_QuestOpenXr_setActivity(JNIEnv* env, jobject, jobject activity) {
    std::scoped_lock lock(g_mutex);
    if (g_activity != nullptr) env->DeleteGlobalRef(g_activity);
    g_activity = activity != nullptr ? env->NewGlobalRef(activity) : nullptr;
    if (g_vm == nullptr) env->GetJavaVM(&g_vm);
}

extern "C" JNIEXPORT void JNICALL
Java_org_citra_citra_emu_QuestOpenXr_setResumed(JNIEnv*, jobject, jboolean resumed) {
    std::scoped_lock lock(g_mutex);
    g_resumed = resumed == JNI_TRUE;
}

namespace QuestOpenXR {
void SetActivity(void*, void*) {}
void SetResumed(bool) {}

bool EnsureSession(EGLDisplay display, EGLConfig config, EGLContext context) {
    std::scoped_lock lock(g_mutex);
    if (g_session != XR_NULL_HANDLE) return true;
    if (!g_resumed || g_activity == nullptr || g_vm == nullptr) return false;
    if (!HasExtension(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) ||
        !HasExtension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
        Log("Required Android/OpenGL ES OpenXR extension is unavailable");
        return false;
    }

    const char* extensions[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME
    };
    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = g_vm;
    android_info.applicationActivity = g_activity;

    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    instance_info.next = &android_info;
    instance_info.enabledExtensionCount = 2;
    instance_info.enabledExtensionNames = extensions;
    std::strncpy(instance_info.applicationInfo.applicationName, "Azahar Quest", XR_MAX_APPLICATION_NAME_SIZE - 1);
    instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    if (XR_FAILED(xrCreateInstance(&instance_info, &g_instance))) {
        Log("xrCreateInstance failed");
        g_instance = XR_NULL_HANDLE;
        return false;
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(g_instance, &system_info, &g_system))) {
        Log("xrGetSystem failed");
        DestroySessionLocked();
        return false;
    }

    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_requirements{};
    if (XR_FAILED(xrGetInstanceProcAddr(g_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements))) || !get_requirements) {
        Log("OpenGL ES graphics requirements function unavailable");
        DestroySessionLocked();
        return false;
    }
    XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (XR_FAILED(get_requirements(g_instance, g_system, &requirements))) {
        Log("OpenGL ES graphics requirements query failed");
        DestroySessionLocked();
        return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = display;
    binding.config = config;
    binding.context = context;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = g_system;
    if (XR_FAILED(xrCreateSession(g_instance, &session_info, &g_session))) {
        Log("xrCreateSession failed");
        DestroySessionLocked();
        return false;
    }

    XrReferenceSpaceCreateInfo local_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    local_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    local_info.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateReferenceSpace(g_session, &local_info, &g_local_space))) {
        Log("Failed to create LOCAL reference space");
        DestroySessionLocked();
        return false;
    }
    XrReferenceSpaceCreateInfo view_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    view_info.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateReferenceSpace(g_session, &view_info, &g_view_space))) {
        Log("Failed to create VIEW reference space");
        DestroySessionLocked();
        return false;
    }
    Log("OpenXR OpenGL ES session and head reference spaces created");
    PollEventsLocked();
    return true;
}

bool BeginFrameAndLocate(QuestHeadPose& pose) {
    std::scoped_lock lock(g_mutex);
    pose.valid = false;
    if (g_session == XR_NULL_HANDLE) return false;
    PollEventsLocked();
    if (!g_session_running) return false;

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_session, &wait_info, &frame_state))) return false;
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_session, &begin_info))) return false;
    g_frame_begun = true;
    g_display_time = frame_state.predictedDisplayTime;

    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(g_view_space, g_local_space, g_display_time, &location)) &&
        (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        pose.position[0] = location.pose.position.x;
        pose.position[1] = location.pose.position.y;
        pose.position[2] = location.pose.position.z;
        pose.orientation[0] = location.pose.orientation.x;
        pose.orientation[1] = location.pose.orientation.y;
        pose.orientation[2] = location.pose.orientation.z;
        pose.orientation[3] = location.pose.orientation.w;
        pose.valid = true;
    }
    return true;
}

void EndFrame() {
    std::scoped_lock lock(g_mutex);
    if (!g_frame_begun || g_session == XR_NULL_HANDLE) return;
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = g_display_time;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = 0;
    end_info.layers = nullptr;
    xrEndFrame(g_session, &end_info);
    g_frame_begun = false;
}

void Shutdown() {
    std::scoped_lock lock(g_mutex);
    DestroySessionLocked();
}
} // namespace QuestOpenXR
