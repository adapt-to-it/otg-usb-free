package com.example.otgcam

import android.content.Context
import android.content.pm.ApplicationInfo
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

/**
 * Wrapper Kotlin del modulo nativo `otgcam_native`.
 *
 * Stato attuale (step 3): context Vulkan + swapchain + render di un clear
 * color sul Surface fornito. L'upload dei frame e i compute shader saranno
 * aggiunti negli step successivi.
 *
 * Pattern d'uso tipico:
 *   VulkanRenderer.init(context)
 *   if (VulkanRenderer.isReady) {
 *       // SurfaceHolder.Callback:
 *       //   surfaceCreated/Changed -> VulkanRenderer.attachSurface(holder.surface)
 *       //   surfaceDestroyed       -> VulkanRenderer.detachSurface()
 *       VulkanRenderer.renderClear(1f, 0f, 0f, 1f)  // schermo rosso
 *   } else {
 *       // fallback path: cam.addSurface(holder.surface)
 *   }
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
     * @return true se Vulkan è utilizzabile sul device.
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

    /**
     * Aggancia il Surface fornito alla pipeline Vulkan, creando swapchain
     * e command pool. Ricreabile: chiamandolo di nuovo con un Surface diverso
     * (o lo stesso dopo un resize) ricrea le risorse.
     *
     * @return true se la swapchain è stata creata con successo.
     */
    @Synchronized
    fun attachSurface(surface: Surface): Boolean {
        if (!nativeLoaded) return false
        return nativeAttachSurface(surface)
    }

    @Synchronized
    fun detachSurface() {
        if (!nativeLoaded) return
        nativeDetachSurface()
    }

    /** Renderizza un singolo frame con clear color e present. */
    fun renderClear(r: Float, g: Float, b: Float, a: Float = 1f): Boolean {
        if (!nativeLoaded) return false
        return nativeRenderClear(r, g, b, a)
    }

    /**
     * Carica un frame YUYV (YUY2) nella texture intermedia. Il buffer DEVE
     * essere un direct ByteBuffer; il native legge da GetDirectBufferAddress.
     */
    fun uploadFrameYUYV(buffer: ByteBuffer, width: Int, height: Int): Boolean {
        if (!nativeLoaded) return false
        if (!buffer.isDirect) {
            Log.e(TAG, "uploadFrameYUYV: buffer is not direct")
            return false
        }
        return nativeUploadFrameYUYV(buffer, width, height)
    }

    /**
     * Renderizza l'ultimo frame uploadato applicando aspect/rotation/mirror
     * persistiti via setTransform / setAspect. Se non c'e' frame, render
     * di clear color rosso.
     */
    fun renderFrame(): Boolean {
        if (!nativeLoaded) return false
        return nativeRenderFrame()
    }

    /** aspect: 0=fit, 1=fill, 2=stretch */
    fun setAspect(aspect: Int) {
        if (!nativeLoaded) return
        nativeSetAspect(aspect)
    }

    /**
     * Parametri post-process del compute shader.
     * sharpen/denoise/defog: 0..100 (intensita').
     * defogOn: se false, defog ignorato indipendentemente dal valore.
     */
    fun setEnhancement(sharpen: Int, denoise: Int, defog: Int, defogOn: Boolean) {
        if (!nativeLoaded) return
        nativeSetEnhancement(sharpen, denoise, defog, defogOn)
    }

    /**
     * Trasformazione user-space applicata dal compute shader sul frame.
     * scaleX/scaleY: 1.0 = nessuno scaling. rotateDeg: 0/90/180/270.
     * Se mirrorX=true il sample viene specchiato orizzontalmente, idem Y.
     */
    fun setTransform(scaleX: Float, scaleY: Float, rotateDeg: Float,
                     mirrorX: Boolean, mirrorY: Boolean) {
        if (!nativeLoaded) return
        nativeSetTransform(scaleX, scaleY, rotateDeg, mirrorX, mirrorY)
    }

    fun versionString(): String? = if (nativeLoaded) nativeVersionString() else null

    @JvmStatic private external fun nativeVersionString(): String
    @JvmStatic private external fun nativeInit(enableValidation: Boolean): String?
    @JvmStatic private external fun nativeShutdown()
    @JvmStatic private external fun nativeIsReady(): Boolean
    @JvmStatic private external fun nativeAttachSurface(surface: Surface): Boolean
    @JvmStatic private external fun nativeDetachSurface()
    @JvmStatic private external fun nativeRenderClear(r: Float, g: Float, b: Float, a: Float): Boolean
    @JvmStatic private external fun nativeSetTransform(
        scaleX: Float, scaleY: Float, rotateDeg: Float,
        mirrorX: Boolean, mirrorY: Boolean
    )
    @JvmStatic private external fun nativeUploadFrameYUYV(buffer: ByteBuffer, width: Int, height: Int): Boolean
    @JvmStatic private external fun nativeRenderFrame(): Boolean
    @JvmStatic private external fun nativeSetAspect(aspect: Int)
    @JvmStatic private external fun nativeSetEnhancement(
        sharpen: Int, denoise: Int, defog: Int, defogOn: Boolean
    )
}
