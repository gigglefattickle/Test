// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version.

package org.citra.citra_emu

import android.content.Context
import android.app.Activity

/**
 * Small Android-side entry point for the Khronos OpenXR loader.
 *
 * This initializes the loader only. An OpenXR instance/session, swapchains,
 * frame loop, tracking and stereo presentation are implemented separately.
 */
object QuestOpenXr {
    private const val XR_SUCCESS = 0

    external fun initializeLoader(context: Context): Int
    external fun setActivity(activity: Activity?)
    external fun setResumed(resumed: Boolean)
    external fun getHeadPose(): FloatArray

    fun initialize(context: Context): Boolean =
        initializeLoader(context.applicationContext) == XR_SUCCESS

    /** Returns [x, y, z, qx, qy, qz, qw, valid] in the OpenXR LOCAL space. */
    fun headPose(): FloatArray = getHeadPose()
}
