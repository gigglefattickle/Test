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

namespace QuestOpenXR {
bool EnsureSession(EGLDisplay display, EGLConfig config, EGLContext context);
bool BeginFrameAndLocate(QuestHeadPose& pose);
void PresentStereoFrame(unsigned int left_texture, unsigned int right_texture, uint32_t source_width, uint32_t source_height);
void GetHeadPose(QuestHeadPose& pose);
void EndFrame();
void Shutdown();
} // namespace QuestOpenXR
