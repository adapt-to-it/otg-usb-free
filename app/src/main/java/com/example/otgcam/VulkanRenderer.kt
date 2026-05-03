package com.example.otgcam

import android.content.Context
import android.content.pm.ApplicationInfo
import android.util.Log

/**
 * Wrapper Kotlin del modulo nativo `otgcam_native`.
 *
 * Stato attuale (step 2): espone init/shutdown del VulkanContext.
 * Lo swapchain e l'upload dei frame verranno aggiunti negli step successivi.
 *
 * Pattern d'uso:
 *   VulkanRenderer.init(context)
 *   if (VulkanRenderer.isReady) { ... pipeline Vulkan ... }
 *   else { ... fallback Surface ... }
 *   ...
 *   VulkanRenderer.shutdown()
 */
object VulkanRenderer {

    private const val TAG = "VulkanRenderer"
    private const val LIB_NAME = "otgcam_native"

    /** True se la libreria nativa è stata caricata con successo all'avvio. */
    val nativeLoaded: Boolean

    /**
     * Sintesi del device Vulkan attivo (es. "Adreno (TM) 660|api_1.1.128|qf_0|validation_off"),
     * disponibile dopo init() andato a buon fine. Null se Vulkan non è attivo.
     */
    @Volatile
    var deviceSummary: String? = null
        private set

    /** True se il VulkanContext nativo è stato creato e non ancora distrutto. */
    val isReady: Boolean
        get() = nativeLoaded && nativeIsReady()

    init {
        var loaded = false
        try {
            System.loadLibrary(LIB_NAME)
            loaded = true
            Log.i(TAG, "Loaded native library '$LIB_NAME' (${nativeVersionString()})")
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to load native library '$LIB_NAME'", t)
        }
        nativeLoaded = loaded
    }

    /**
     * Inizializza il VulkanContext. Idempotente.
     *
     * @return true se Vulkan è utilizzabile sul device, false se occorre
     *         cadere sul path Surface (fallback).
     */
    @Synchronized
    fun init(context: Context): Boolean {
        if (!nativeLoaded) return false
        val debug = (context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
        val summary = nativeInit(debug)
        deviceSummary = summary
        if (summary != null) {
            Log.i(TAG, "Vulkan device: $summary (validation requested=$debug)")
            return true
        }
        Log.w(TAG, "Vulkan init failed; falling back to Surface path")
        return false
    }

    @Synchronized
    fun shutdown() {
        if (!nativeLoaded) return
        nativeShutdown()
        deviceSummary = null
    }

    fun versionString(): String? = if (nativeLoaded) nativeVersionString() else null

    @JvmStatic private external fun nativeVersionString(): String
    @JvmStatic private external fun nativeInit(enableValidation: Boolean): String?
    @JvmStatic private external fun nativeShutdown()
    @JvmStatic private external fun nativeIsReady(): Boolean
}
