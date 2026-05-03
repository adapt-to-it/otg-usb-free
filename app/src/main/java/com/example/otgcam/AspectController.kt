package com.example.otgcam

import android.util.Log

/**
 * Calcola la trasformazione (scale + rotate + mirror) che il compute shader
 * Vulkan applica al sample del frame, in base ai settings utente.
 *
 * Convenzione del modulo nativo:
 *   src_uv = (out_uv - 0.5) * scale + 0.5
 *
 *   - scale == 1.0  → l'asse è mappato 1:1 (la dimensione del frame copre
 *                     esattamente la view in quel asse)
 *   - scale  < 1.0  → solo la frazione centrale del frame e' campionata
 *                     in quel asse → la sorgente "esce" dalla view (crop)
 *   - scale  > 1.0  → out_uv copre solo una frazione del frame in quel
 *                     asse → bande nere (letterbox / pillarbox)
 *
 * La rotazione user-space (0/90/180/270) viene gestita lato shader; qui
 * compensiamo solo l'aspect ratio passando il frame `srcW×srcH` con i lati
 * scambiati quando la rotazione e' 90/270 (cosi' il calcolo fit/fill lavora
 * sull'orientamento finale visualizzato).
 *
 * Sostituisce la logica interna di `AspectRatioSurfaceView` (libreria
 * UVCAndroid) che modificava le `layoutParams`: ora la SurfaceView sta
 * sempre `match_parent` e la composizione avviene nello shader.
 */
class AspectController(private val settings: Settings) {

    private companion object {
        const val TAG = "AspectController"
    }

    /**
     * @param viewW  larghezza in pixel del SurfaceView su schermo
     * @param viewH  altezza in pixel del SurfaceView su schermo
     * @param frameW larghezza nativa del frame sorgente (non ruotato)
     * @param frameH altezza nativa del frame sorgente
     */
    fun update(viewW: Int, viewH: Int, frameW: Int, frameH: Int) {
        if (!VulkanRenderer.isReady) return
        if (viewW <= 0 || viewH <= 0 || frameW <= 0 || frameH <= 0) return

        val rotation = settings.rotation
        // Per la stima dell'aspect post-rotazione, scambiamo i lati su 90/270.
        var srcW = frameW
        var srcH = frameH
        if (rotation == 90 || rotation == 270) {
            val tmp = srcW; srcW = srcH; srcH = tmp
        }
        val viewAspect = viewW.toFloat() / viewH.toFloat()
        val srcAspect  = srcW.toFloat() / srcH.toFloat()

        val (scaleX, scaleY) = when (settings.aspect) {
            "stretch" -> 1f to 1f
            "fill"    -> {
                if (srcAspect > viewAspect) (viewAspect / srcAspect) to 1f
                else                         1f to (srcAspect / viewAspect)
            }
            else /* "fit" */ -> {
                if (srcAspect > viewAspect) 1f to (srcAspect / viewAspect)
                else                         (viewAspect / srcAspect) to 1f
            }
        }

        VulkanRenderer.setTransform(
            scaleX = scaleX,
            scaleY = scaleY,
            rotateDeg = rotation.toFloat(),
            mirrorX = settings.mirrorH,
            mirrorY = settings.mirrorV
        )
        Log.d(
            TAG,
            "update view=${viewW}x$viewH frame=${frameW}x$frameH " +
                "aspect=${settings.aspect} rot=$rotation mirrorH=${settings.mirrorH} " +
                "mirrorV=${settings.mirrorV} -> scale=($scaleX,$scaleY)"
        )
    }
}
