package com.example.otgcam

import android.util.Log

/**
 * Wrapper Kotlin del modulo nativo `otgcam_native`.
 *
 * In questo step espone solo `versionString()` per verificare che
 * il caricamento della libreria nativa funzioni sui 4 ABI configurati.
 * Le API reali (init Vulkan, upload frame, set params, ecc.) saranno
 * aggiunte negli step successivi del piano.
 */
object VulkanRenderer {

    private const val TAG = "VulkanRenderer"
    private const val LIB_NAME = "otgcam_native"

    /** True se la libreria nativa è stata caricata con successo. */
    val nativeLoaded: Boolean

    init {
        var loaded = false
        try {
            System.loadLibrary(LIB_NAME)
            loaded = true
            Log.i(TAG, "Loaded native library '$LIB_NAME'")
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to load native library '$LIB_NAME'", t)
        }
        nativeLoaded = loaded
    }

    /**
     * Versione del modulo nativo. Ritorna `null` se la libreria non è stata
     * caricata (es. ABI non supportato dal device).
     */
    fun versionString(): String? = if (nativeLoaded) nativeVersionString() else null

    @JvmStatic
    private external fun nativeVersionString(): String
}
