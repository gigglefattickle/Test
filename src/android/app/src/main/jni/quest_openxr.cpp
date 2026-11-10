// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version.

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <GLES3/gl3.h>

#include <android/log.h>
#include <jni.h>

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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
XrSessionState g_session_state{XR_SESSION_STATE_UNKNOWN};
bool g_frame_begun{};
bool g_frame_should_render{};
bool g_quad_layer_ready{};
XrSwapchain g_swapchain{XR_NULL_HANDLE};
std::vector<XrSwapchainImageOpenGLESKHR> g_swapchain_images;
constexpr uint32_t g_swapchain_width = 2048;
constexpr uint32_t g_swapchain_height = 1152;
GLuint g_copy_framebuffer{};
QuestHeadPose g_latest_pose{};
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

bool CreateSwapchainLocked() {
    uint32_t format_count{};
    if (XR_FAILED(xrEnumerateSwapchainFormats(g_session, 0, &format_count, nullptr)) || !format_count) return false;
    std::vector<int64_t> formats(format_count);
    if (XR_FAILED(xrEnumerateSwapchainFormats(g_session, format_count, &format_count, formats.data()))) return false;
    if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(GL_RGBA8)) == formats.end()) {
        Log("OpenXR runtime does not support GL_RGBA8 swapchain images");
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = GL_RGBA8;
    info.sampleCount = 1;
    info.width = g_swapchain_width;
    info.height = g_swapchain_height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g_session, &info, &g_swapchain))) return false;
    uint32_t image_count{};
    if (XR_FAILED(xrEnumerateSwapchainImages(g_swapchain, 0, &image_count, nullptr)) || !image_count) return false;
    g_swapchain_images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(g_swapchain, image_count, &image_count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(g_swapchain_images.data())))) return false;
    glGenFramebuffers(1, &g_copy_framebuffer);
    return g_copy_framebuffer != 0;
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
    if (g_swapchain != XR_NULL_HANDLE) xrDestroySwapchain(g_swapchain);
    if (g_copy_framebuffer) glDeleteFramebuffers(1, &g_copy_framebuffer);
    g_swapchain = XR_NULL_HANDLE;
    g_copy_framebuffer = 0;
    g_swapchain_images.clear();
    g_quad_layer_ready = false;
    if (g_view_space != XR_NULL_HANDLE) xrDestroySpace(g_view_space);
    if (g_local_space != XR_NULL_HANDLE) xrDestroySpace(g_local_space);
    if (g_session != XR_NULL_HANDLE) xrDestroySession(g_session);
    if (g_instance != XR_NULL_HANDLE) xrDestroyInstance(g_instance);
    g_view_space = XR_NULL_HANDLE;
    g_local_space = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_session_state = XR_SESSION_STATE_UNKNOWN;
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
            g_session_state = changed->state;
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
    // READY may arrive while Android is paused. Start the session once the activity resumes.
    if (g_session_state == XR_SESSION_STATE_READY && g_resumed && !g_session_running &&
        g_session != XR_NULL_HANDLE) {
        XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
        begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (XR_SUCCEEDED(xrBeginSession(g_session, &begin_info))) {
            g_session_running = true;
            Log("OpenXR session started");
        }
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
    if (!CreateSwapchainLocked()) {
        Log("Failed to create OpenXR game-screen swapchain");
        DestroySessionLocked();
        return false;
    }
    Log("OpenXR OpenGL ES session, head tracking, and game-screen swapchain created");
    PollEventsLocked();
    return true;
}

bool BeginFrameAndLocate(QuestHeadPose& pose) {
    std::scoped_lock lock(g_mutex);
    pose.valid = false;
    g_latest_pose = {};
    if (g_session == XR_NULL_HANDLE) return false;
    PollEventsLocked();
    if (!g_resumed || !g_session_running) return false;

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_session, &wait_info, &frame_state))) return false;
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_session, &begin_info))) return false;
    g_frame_begun = true;
    g_frame_should_render = frame_state.shouldRender == XR_TRUE;
    g_quad_layer_ready = false;
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
    g_latest_pose = pose;
    return true;
}

void PresentFrame(EGLint source_width, EGLint source_height) {
    std::scoped_lock lock(g_mutex);
    if (!g_frame_begun || !g_frame_should_render || g_swapchain == XR_NULL_HANDLE ||
        source_width <= 0 || source_height <= 0) return;

    uint32_t image_index{};
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_swapchain, &acquire, &image_index))) return;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(g_swapchain, &wait))) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_swapchain, &release);
        return;
    }

    GLint old_read{}, old_draw{}, old_read_buffer{};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g_swapchain_images[image_index].image, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glBlitFramebuffer(0, 0, source_width, source_height, 0, 0,
                          g_swapchain_width, g_swapchain_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glFlush();
        g_quad_layer_ready = glGetError() == GL_NO_ERROR;
    } else {
        Log("OpenXR copy framebuffer is incomplete");
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
    glReadBuffer(static_cast<GLenum>(old_read_buffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(xrReleaseSwapchainImage(g_swapchain, &release))) {
        g_quad_layer_ready = false;
        Log("Failed to release OpenXR swapchain image");
    }
}

void EndFrame() {
    std::scoped_lock lock(g_mutex);
    if (!g_frame_begun || g_session == XR_NULL_HANDLE) return;
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = g_display_time;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad)
    };
    if (g_quad_layer_ready) {
        quad.space = g_local_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_swapchain;
        quad.subImage.imageRect.offset = {0, 0};
        quad.subImage.imageRect.extent = {static_cast<int32_t>(g_swapchain_width),
                                          static_cast<int32_t>(g_swapchain_height)};
        quad.pose.orientation.w = 1.0f;
        quad.pose.position = {0.0f, 1.35f, -2.0f};
        quad.size = {2.4f, 1.35f};
    }
    end_info.layerCount = g_quad_layer_ready ? 1u : 0u;
    end_info.layers = g_quad_layer_ready ? layers : nullptr;
    xrEndFrame(g_session, &end_info);
    g_frame_should_render = false;
    g_quad_layer_ready = false;
    g_frame_begun = false;
}

void GetHeadPose(QuestHeadPose& pose) {
    std::scoped_lock lock(g_mutex);
    pose = g_latest_pose;
}

void Shutdown() {
    std::scoped_lock lock(g_mutex);
    if (g_session != XR_NULL_HANDLE && g_session_running) {
        xrRequestExitSession(g_session);
        for (int attempt = 0; attempt < 100 && g_session_running; ++attempt) {
            PollEventsLocked();
            if (g_session_running) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (g_session_running) {
            Log("OpenXR exit was not acknowledged; deferring session destruction");
            return;
        }
    }
    DestroySessionLocked();
    g_latest_pose = {};
}
} // namespace QuestOpenXR

extern "C" JNIEXPORT jfloatArray JNICALL
Java_org_citra_citra_emu_QuestOpenXr_getHeadPose(JNIEnv* env, jobject) {
    QuestHeadPose pose{};
    QuestOpenXR::GetHeadPose(pose);
    const jfloat values[8] = {
        pose.position[0], pose.position[1], pose.position[2],
        pose.orientation[0], pose.orientation[1], pose.orientation[2],
        pose.orientation[3], pose.valid ? 1.0f : 0.0f
    };
    jfloatArray result = env->NewFloatArray(8);
    if (result != nullptr) env->SetFloatArrayRegion(result, 0, 8, values);
    return result;
}
