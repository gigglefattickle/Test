// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version.

package org.citra.citra_emu

import android.content.Context
import android.app.Activity

/**
 * Android lifecycle and pose bridge for the native Quest OpenXR implementation.
 * Native code owns the OpenXR instance/session, GL swapchain, and composition layer.
 */
object QuestOpenXr {
    private const val XR_SUCCESS = 0

    external fun initializeLoader(context: Context): Int
    external fun setActivity(activity: Activity?)
    external fun setResumed(resumed: Boolean)
    external fun getHeadPose(): FloatArray
    external fun getControllerStates(): FloatArray

    fun initialize(context: Context): Boolean =
        initializeLoader(context.applicationContext) == XR_SUCCESS

    /** Returns [x, y, z, qx, qy, qz, qw, valid] in the OpenXR LOCAL space. */
    fun headPose(): FloatArray = getHeadPose()

    /** Two 21-float records: grip pose, aim pose, buttons, trigger, squeeze, stick, validity. */
    fun controllerStates(): FloatArray = getControllerStates()
}
