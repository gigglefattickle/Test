// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version.
#pragma once

#include <EGL/egl.h>
#include <cstdint>

struct QuestHeadPose {
    float position[3]{};
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    bool valid{};
};

struct QuestControllerState {
    float grip_position[3]{};
    float grip_orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float aim_position[3]{};
    float aim_orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    uint32_t buttons{};
    float trigger{};
    float squeeze{};
    float stick_x{};
    float stick_y{};
    bool pose_valid{};
};

namespace QuestOpenXR {
bool EnsureSession(EGLDisplay display, EGLConfig config, EGLContext context);
bool BeginFrameAndLocate(QuestHeadPose& pose);
void PresentStereoFrame(unsigned int left_texture, unsigned int right_texture, uint32_t source_width, uint32_t source_height);
void GetHeadPose(QuestHeadPose& pose);
void GetControllerStates(QuestControllerState (&states)[2]);
void EndFrame();
void Shutdown();
} // namespace QuestOpenXR
