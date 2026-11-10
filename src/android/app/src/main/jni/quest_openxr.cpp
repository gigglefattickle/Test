// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version.

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <GLES3/gl3.h>

#include <algorithm>

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
uint32_t g_swapchain_width{1440};
uint32_t g_swapchain_height{1600};
std::array<XrViewConfigurationView, 2> g_view_configurations{
    XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
    XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
GLuint g_read_framebuffer{};
GLuint g_draw_framebuffer{};
std::array<XrCompositionLayerProjectionView, 2> g_projection_views{
    XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
QuestHeadPose g_latest_pose{};

struct ControllerActions {
    XrPath hand_path{XR_NULL_PATH};
    XrAction buttons[6]{};
    XrAction trigger{XR_NULL_HANDLE};
    XrAction squeeze{XR_NULL_HANDLE};
    XrAction stick{XR_NULL_HANDLE};
    XrAction grip_pose{XR_NULL_HANDLE};
    XrAction aim_pose{XR_NULL_HANDLE};
    XrSpace grip_space{XR_NULL_HANDLE};
    XrSpace aim_space{XR_NULL_HANDLE};
    QuestControllerState state{};
};
XrActionSet g_action_set{XR_NULL_HANDLE};
std::array<ControllerActions, 2> g_controllers{};

bool CreateControllerActionsLocked() {
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(set_info.actionSetName, "quest_controller", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(set_info.localizedActionSetName, "Quest Controllers",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (XR_FAILED(xrCreateActionSet(g_instance, &set_info, &g_action_set))) return false;

    const char* hands[] = {"/user/hand/left", "/user/hand/right"};
    const char* buttons[2][6] = {
        {"/input/x/click", "/input/y/click", "/input/trigger/click",
         "/input/squeeze/click", "/input/thumbstick/click", "/input/menu/click"},
        {"/input/a/click", "/input/b/click", "/input/trigger/click",
         "/input/squeeze/click", "/input/thumbstick/click", "/input/menu/click"}
    };
    const char* button_names[] = {"face_primary", "face_secondary", "trigger_click",
                                  "squeeze_click", "stick_click", "menu_click"};
    const char* analog_paths[] = {"/input/trigger/value", "/input/squeeze/value",
                                  "/input/thumbstick"};
    const char* pose_paths[] = {"/input/grip/pose", "/input/aim/pose"};
    for (uint32_t hand = 0; hand < 2; ++hand) {
        auto& c = g_controllers[hand];
        if (XR_FAILED(xrStringToPath(g_instance, hands[hand], &c.hand_path))) return false;
        for (uint32_t i = 0; i < 6; ++i) {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            info.countSubactionPaths = 1;
            info.subactionPaths = &c.hand_path;
            std::strncpy(info.actionName, button_names[i], XR_MAX_ACTION_NAME_SIZE - 1);
            std::strncpy(info.localizedActionName, button_names[i],
                         XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
            if (XR_FAILED(xrCreateAction(g_action_set, &info, &c.buttons[i]))) return false;
        }
        XrAction* analog_actions[] = {&c.trigger, &c.squeeze, &c.stick};
        const XrActionType analog_types[] = {XR_ACTION_TYPE_FLOAT_INPUT,
            XR_ACTION_TYPE_FLOAT_INPUT, XR_ACTION_TYPE_VECTOR2F_INPUT};
        for (uint32_t i = 0; i < 3; ++i) {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            info.actionType = analog_types[i];
            info.countSubactionPaths = 1;
            info.subactionPaths = &c.hand_path;
            std::strncpy(info.actionName, i == 0 ? "trigger_value" :
                         i == 1 ? "squeeze_value" : "thumbstick", XR_MAX_ACTION_NAME_SIZE - 1);
            std::strncpy(info.localizedActionName, analog_paths[i],
                         XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
            if (XR_FAILED(xrCreateAction(g_action_set, &info, analog_actions[i]))) return false;
        }
        XrAction* pose_actions[] = {&c.grip_pose, &c.aim_pose};
        for (uint32_t i = 0; i < 2; ++i) {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            info.actionType = XR_ACTION_TYPE_POSE_INPUT;
            info.countSubactionPaths = 1;
            info.subactionPaths = &c.hand_path;
            std::strncpy(info.actionName, i == 0 ? "grip_pose" : "aim_pose",
                         XR_MAX_ACTION_NAME_SIZE - 1);
            std::strncpy(info.localizedActionName, pose_paths[i],
                         XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
            if (XR_FAILED(xrCreateAction(g_action_set, &info, pose_actions[i]))) return false;
        }
    }

    XrPath profile{};
    if (XR_FAILED(xrStringToPath(g_instance, "/interaction_profiles/oculus/touch_controller",
                                 &profile))) return false;
    std::vector<XrActionSuggestedBinding> bindings;
    for (uint32_t hand = 0; hand < 2; ++hand) {
        auto& c = g_controllers[hand];
        for (uint32_t i = 0; i < 6; ++i) {
            std::string path = std::string(hands[hand]) + buttons[hand][i];
            XrPath input{};
            if (XR_FAILED(xrStringToPath(g_instance, path.c_str(), &input))) return false;
            bindings.push_back({c.buttons[i], input});
        }
        for (uint32_t i = 0; i < 3; ++i) {
            std::string path = std::string(hands[hand]) + analog_paths[i];
            XrPath input{};
            if (XR_FAILED(xrStringToPath(g_instance, path.c_str(), &input))) return false;
            bindings.push_back({i == 0 ? c.trigger : i == 1 ? c.squeeze : c.stick, input});
        }
        for (uint32_t i = 0; i < 2; ++i) {
            std::string path = std::string(hands[hand]) + pose_paths[i];
            XrPath input{};
            if (XR_FAILED(xrStringToPath(g_instance, path.c_str(), &input))) return false;
            bindings.push_back({i == 0 ? c.grip_pose : c.aim_pose, input});
        }
    }
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = profile;
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    if (XR_FAILED(xrSuggestInteractionProfileBindings(g_instance, &suggested))) return false;
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_action_set;
    if (XR_FAILED(xrAttachSessionActionSets(g_session, &attach))) return false;

    for (auto& c : g_controllers) {
        XrActionSpaceCreateInfo grip{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        grip.action = c.grip_pose;
        grip.subactionPath = c.hand_path;
        grip.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateActionSpace(g_session, &grip, &c.grip_space))) return false;
        XrActionSpaceCreateInfo aim{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        aim.action = c.aim_pose;
        aim.subactionPath = c.hand_path;
        aim.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateActionSpace(g_session, &aim, &c.aim_space))) return false;
    }
    Log("Quest Touch controller actions created");
    return true;
}

void SyncControllerActionsLocked() {
    if (g_action_set == XR_NULL_HANDLE || !g_session_running) return;
    XrActiveActionSet active{g_action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(g_session, &sync))) return;
    for (auto& c : g_controllers) {
        c.state = {};
        for (uint32_t i = 0; i < 6; ++i) {
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
            get.action = c.buttons[i];
            get.subactionPath = c.hand_path;
            XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &get, &value)) &&
                value.isActive && value.currentState) c.state.buttons |= (1u << i);
        }
        XrActionStateGetInfo trigger_get{XR_TYPE_ACTION_STATE_GET_INFO};
        trigger_get.action = c.trigger;
        trigger_get.subactionPath = c.hand_path;
        XrActionStateFloat trigger_value{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &trigger_get, &trigger_value)) &&
            trigger_value.isActive) c.state.trigger = trigger_value.currentState;
        XrActionStateGetInfo squeeze_get{XR_TYPE_ACTION_STATE_GET_INFO};
        squeeze_get.action = c.squeeze;
        squeeze_get.subactionPath = c.hand_path;
        XrActionStateFloat squeeze_value{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &squeeze_get, &squeeze_value)) &&
            squeeze_value.isActive) c.state.squeeze = squeeze_value.currentState;
        XrActionStateGetInfo stick_get{XR_TYPE_ACTION_STATE_GET_INFO};
        stick_get.action = c.stick;
        stick_get.subactionPath = c.hand_path;
        XrActionStateVector2f stick_value{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &stick_get, &stick_value)) &&
            stick_value.isActive) {
            c.state.stick_x = stick_value.currentState.x;
            c.state.stick_y = stick_value.currentState.y;
        }
        XrSpaceLocation grip{XR_TYPE_SPACE_LOCATION};
        XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(c.grip_space, g_local_space, g_display_time, &grip)) &&
            (grip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (grip.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            c.state.grip_position[0] = grip.pose.position.x;
            c.state.grip_position[1] = grip.pose.position.y;
            c.state.grip_position[2] = grip.pose.position.z;
            c.state.grip_orientation[0] = grip.pose.orientation.x;
            c.state.grip_orientation[1] = grip.pose.orientation.y;
            c.state.grip_orientation[2] = grip.pose.orientation.z;
            c.state.grip_orientation[3] = grip.pose.orientation.w;
        }
        if (XR_SUCCEEDED(xrLocateSpace(c.aim_space, g_local_space, g_display_time, &aim)) &&
            (aim.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (aim.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            c.state.aim_position[0] = aim.pose.position.x;
            c.state.aim_position[1] = aim.pose.position.y;
            c.state.aim_position[2] = aim.pose.position.z;
            c.state.aim_orientation[0] = aim.pose.orientation.x;
            c.state.aim_orientation[1] = aim.pose.orientation.y;
            c.state.aim_orientation[2] = aim.pose.orientation.z;
            c.state.aim_orientation[3] = aim.pose.orientation.w;
            c.state.pose_valid = true;
        }
    }
}

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
    uint32_t view_count{};
    if (XR_FAILED(xrEnumerateViewConfigurationViews(
            g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &view_count, nullptr)) || view_count != 2) {
        Log("Quest runtime did not report two primary stereo views");
        return false;
    }
    if (XR_FAILED(xrEnumerateViewConfigurationViews(
            g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            view_count, &view_count, g_view_configurations.data()))) {
        Log("Failed to enumerate stereo view dimensions");
        return false;
    }
    g_swapchain_width = std::max(g_view_configurations[0].recommendedImageRectWidth,
                                 g_view_configurations[1].recommendedImageRectWidth);
    g_swapchain_height = std::max(g_view_configurations[0].recommendedImageRectHeight,
                                  g_view_configurations[1].recommendedImageRectHeight);

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
    info.arraySize = 2;
    info.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g_session, &info, &g_swapchain))) return false;

    uint32_t image_count{};
    if (XR_FAILED(xrEnumerateSwapchainImages(g_swapchain, 0, &image_count, nullptr)) || !image_count) return false;
    g_swapchain_images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(g_swapchain, image_count, &image_count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(g_swapchain_images.data())))) return false;

    glGenFramebuffers(1, &g_read_framebuffer);
    glGenFramebuffers(1, &g_draw_framebuffer);
    return g_read_framebuffer != 0 && g_draw_framebuffer != 0;
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
    if (g_read_framebuffer) glDeleteFramebuffers(1, &g_read_framebuffer);
    if (g_draw_framebuffer) glDeleteFramebuffers(1, &g_draw_framebuffer);
    g_swapchain = XR_NULL_HANDLE;
    g_read_framebuffer = 0;
    g_draw_framebuffer = 0;
    g_swapchain_images.clear();
    g_quad_layer_ready = false;
    for (auto& c : g_controllers) {
        if (c.grip_space != XR_NULL_HANDLE) xrDestroySpace(c.grip_space);
        if (c.aim_space != XR_NULL_HANDLE) xrDestroySpace(c.aim_space);
        c.grip_space = XR_NULL_HANDLE;
        c.aim_space = XR_NULL_HANDLE;
        c.state = {};
    }
    if (g_action_set != XR_NULL_HANDLE) xrDestroyActionSet(g_action_set);
    g_action_set = XR_NULL_HANDLE;
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
    if (!CreateControllerActionsLocked()) {
        Log("Failed to create Quest controller actions");
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
    SyncControllerActionsLocked();
    return true;
}

void PresentStereoFrame(GLuint left_texture, GLuint right_texture,
                         uint32_t source_width, uint32_t source_height) {
    std::scoped_lock lock(g_mutex);
    if (!g_frame_begun || !g_frame_should_render || g_swapchain == XR_NULL_HANDLE ||
        left_texture == 0 || right_texture == 0 || source_width == 0 || source_height == 0) return;

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

    while (glGetError() != GL_NO_ERROR) {}
    GLint old_read{}, old_draw{}, old_read_buffer{};
    GLint old_viewport[4]{};
    GLfloat old_clear_color[4]{};
    const GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glGetIntegerv(GL_VIEWPORT, old_viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear_color);
    glDisable(GL_SCISSOR_TEST);
    const GLuint sources[2] = {left_texture, right_texture};
    const uint32_t image_height = std::min(g_swapchain_height, (g_swapchain_width * 3) / 5);
    const uint32_t image_y = (g_swapchain_height - image_height) / 2;
    bool copied = true;

    for (uint32_t eye = 0; eye < 2; ++eye) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_read_framebuffer);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               sources[eye], 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_draw_framebuffer);
        glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  g_swapchain_images[image_index].image, 0, eye);
        const GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        const GLenum draw_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (read_status != GL_FRAMEBUFFER_COMPLETE || draw_status != GL_FRAMEBUFFER_COMPLETE) {
            copied = false;
            Log("Stereo eye framebuffer is incomplete");
            break;
        }
        glViewport(0, 0, g_swapchain_width, g_swapchain_height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlitFramebuffer(0, 0, source_width, source_height,
                          0, image_y, g_swapchain_width, image_y + image_height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        if (glGetError() != GL_NO_ERROR) {
            copied = false;
            Log("Failed copying a 3DS stereo eye into the OpenXR swapchain");
            break;
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
    glReadBuffer(static_cast<GLenum>(old_read_buffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
    glClearColor(old_clear_color[0], old_clear_color[1], old_clear_color[2], old_clear_color[3]);
    if (scissor_was_enabled) glEnable(GL_SCISSOR_TEST);

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(xrReleaseSwapchainImage(g_swapchain, &release))) copied = false;
    if (!copied) return;

    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = g_display_time;
    locate_info.space = g_local_space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{
        XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    uint32_t located_count{};
    if (XR_FAILED(xrLocateViews(g_session, &locate_info, &view_state,
                                static_cast<uint32_t>(views.size()), &located_count, views.data())) ||
        located_count != 2 ||
        (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
        Log("Could not locate both stereo eye views");
        return;
    }

    for (uint32_t eye = 0; eye < 2; ++eye) {
        g_projection_views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        g_projection_views[eye].pose = views[eye].pose;
        g_projection_views[eye].fov = views[eye].fov;
        g_projection_views[eye].subImage.swapchain = g_swapchain;
        g_projection_views[eye].subImage.imageRect.offset = {0, 0};
        g_projection_views[eye].subImage.imageRect.extent = {
            static_cast<int32_t>(g_swapchain_width), static_cast<int32_t>(g_swapchain_height)};
        g_projection_views[eye].subImage.imageArrayIndex = eye;
    }
    g_quad_layer_ready = true;
}


void EndFrame() {
    std::scoped_lock lock(g_mutex);
    if (!g_frame_begun || g_session == XR_NULL_HANDLE) return;

    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = g_local_space;
    projection.viewCount = 2;
    projection.views = g_projection_views.data();
    XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection)
    };

    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = g_display_time;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = g_quad_layer_ready ? 1u : 0u;
    end_info.layers = g_quad_layer_ready ? layers : nullptr;
    if (XR_FAILED(xrEndFrame(g_session, &end_info))) {
        Log("xrEndFrame failed");
    }
    g_frame_should_render = false;
    g_quad_layer_ready = false;
    g_frame_begun = false;
}

void GetHeadPose(QuestHeadPose& pose) {
    std::scoped_lock lock(g_mutex);
    pose = g_latest_pose;
}

void GetControllerStates(QuestControllerState (&states)[2]) {
    std::scoped_lock lock(g_mutex);
    states[0] = g_controllers[0].state;
    states[1] = g_controllers[1].state;
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


extern "C" JNIEXPORT jfloatArray JNICALL
Java_org_citra_citra_emu_QuestOpenXr_getControllerStates(JNIEnv* env, jobject) {
    QuestControllerState states[2]{};
    QuestOpenXR::GetControllerStates(states);
    jfloat values[42]{};
    for (int hand = 0; hand < 2; ++hand) {
        const int offset = hand * 21;
        std::memcpy(values + offset, states[hand].grip_position, 3 * sizeof(float));
        std::memcpy(values + offset + 3, states[hand].grip_orientation, 4 * sizeof(float));
        std::memcpy(values + offset + 7, states[hand].aim_position, 3 * sizeof(float));
        std::memcpy(values + offset + 10, states[hand].aim_orientation, 4 * sizeof(float));
        values[offset + 14] = static_cast<float>(states[hand].buttons);
        values[offset + 15] = states[hand].trigger;
        values[offset + 16] = states[hand].squeeze;
        values[offset + 17] = states[hand].stick_x;
        values[offset + 18] = states[hand].stick_y;
        values[offset + 19] = states[hand].pose_valid ? 1.0f : 0.0f;
        values[offset + 20] = 1.0f;
    }
    jfloatArray result = env->NewFloatArray(42);
    if (result != nullptr) env->SetFloatArrayRegion(result, 0, 42, values);
    return result;
}
