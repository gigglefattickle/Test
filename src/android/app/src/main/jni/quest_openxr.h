// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version.
#pragma once

#include <EGL/egl.h>

struct QuestHeadPose {
    float position[3]{};
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    bool valid{};
};

namespace QuestOpenXR {
void SetActivity(void* env, void* activity);
void SetResumed(bool resumed);
bool EnsureSession(EGLDisplay display, EGLConfig config, EGLContext context);
bool BeginFrameAndLocate(QuestHeadPose& pose);
void EndFrame();
void Shutdown();
} // namespace QuestOpenXR
