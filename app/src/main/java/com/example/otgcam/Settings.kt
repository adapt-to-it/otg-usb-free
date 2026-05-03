package com.example.otgcam

import android.content.Context
import android.content.SharedPreferences

class Settings(ctx: Context) {

    private val sp: SharedPreferences =
        ctx.getSharedPreferences("otgcam_prefs", Context.MODE_PRIVATE)

    var resolution: String
        get() = sp.getString(K_RESOLUTION, "auto") ?: "auto"
        set(v) = sp.edit().putString(K_RESOLUTION, v).apply()

    var fps: Int
        get() = sp.getInt(K_FPS, 0)
        set(v) = sp.edit().putInt(K_FPS, v).apply()

    var format: String
        get() = sp.getString(K_FORMAT, "auto") ?: "auto"
        set(v) = sp.edit().putString(K_FORMAT, v).apply()

    var aspect: String
        get() = sp.getString(K_ASPECT, "fit") ?: "fit"
        set(v) = sp.edit().putString(K_ASPECT, v).apply()

    var rotation: Int
        get() = sp.getInt(K_ROTATION, 0)
        set(v) = sp.edit().putInt(K_ROTATION, v).apply()

    var mirrorH: Boolean
        get() = sp.getBoolean(K_MIRROR_H, false)
        set(v) = sp.edit().putBoolean(K_MIRROR_H, v).apply()

    var mirrorV: Boolean
        get() = sp.getBoolean(K_MIRROR_V, false)
        set(v) = sp.edit().putBoolean(K_MIRROR_V, v).apply()

    var orientation: String
        get() = sp.getString(K_ORIENT, "landscape") ?: "landscape"
        set(v) = sp.edit().putString(K_ORIENT, v).apply()

    var keepScreenOn: Boolean
        get() = sp.getBoolean(K_KEEP_ON, true)
        set(v) = sp.edit().putBoolean(K_KEEP_ON, v).apply()

    var hideUiAuto: Boolean
        get() = sp.getBoolean(K_HIDE_UI, true)
        set(v) = sp.edit().putBoolean(K_HIDE_UI, v).apply()

    var brightnessPercent: Int
        get() = sp.getInt(K_BRIGHTNESS, 50)
        set(v) = sp.edit().putInt(K_BRIGHTNESS, v.coerceIn(0, 100)).apply()

    var contrastPercent: Int
        get() = sp.getInt(K_CONTRAST, 50)
        set(v) = sp.edit().putInt(K_CONTRAST, v.coerceIn(0, 100)).apply()

    var exposurePercent: Int
        get() = sp.getInt(K_EXPOSURE, 50)
        set(v) = sp.edit().putInt(K_EXPOSURE, v.coerceIn(0, 100)).apply()

    fun reset() {
        sp.edit().clear().apply()
    }

    fun resolutionWidth(): Int = parseRes().first
    fun resolutionHeight(): Int = parseRes().second

    private fun parseRes(): Pair<Int, Int> {
        if (resolution == "auto") return 0 to 0
        val parts = resolution.split("x")
        return (parts.getOrNull(0)?.toIntOrNull() ?: 0) to
                (parts.getOrNull(1)?.toIntOrNull() ?: 0)
    }

    private companion object {
        const val K_RESOLUTION = "resolution"
        const val K_FPS = "fps"
        const val K_FORMAT = "format"
        const val K_ASPECT = "aspect"
        const val K_ROTATION = "rotation"
        const val K_MIRROR_H = "mirror_h"
        const val K_MIRROR_V = "mirror_v"
        const val K_ORIENT = "orientation"
        const val K_KEEP_ON = "keep_screen_on"
        const val K_HIDE_UI = "hide_ui_auto"
        const val K_BRIGHTNESS = "brightness_percent"
        const val K_CONTRAST = "contrast_percent"
        const val K_EXPOSURE = "exposure_percent"
    }
}
