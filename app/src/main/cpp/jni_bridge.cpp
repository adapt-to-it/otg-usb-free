#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include "compute_pipeline.h"
#include "frame_uploader.h"
#include "preview_renderer.h"
#include "utils/log.h"
#include "vulkan_context.h"

namespace {

// Stato globale del modulo nativo. Una sola istanza per processo.
struct NativeState {
    otgcam::VulkanContext       context;
    otgcam::PreviewRenderer     renderer;
    otgcam::FrameUploader       uploader;
    otgcam::PostProcessPipeline pp;
    ANativeWindow*              window = nullptr;
    bool                        have_frame = false;
    bool                        compute_ready = false;

    // Enhancement params (0..1).
    float    enh_sharpen = 0.0f;
    float    enh_denoise = 0.0f;
    float    enh_defog   = 0.0f;
    uint32_t enh_defog_enabled = 0;

    // Trasformazione richiesta dall'AspectController (Kotlin).
    float scale_x   = 1.0f;
    float scale_y   = 1.0f;
    float rotate_deg = 0.0f;
    bool  mirror_x  = false;
    bool  mirror_y  = false;
    int   aspect   = 0;  // 0=fit, 1=fill, 2=stretch
};

std::mutex                    g_state_mutex;
std::unique_ptr<NativeState>  g_state;

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeVersionString(JNIEnv* env, jclass) {
    return env->NewStringUTF("otgcam_native v0.3 (step 3: swapchain + clear)");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeInit(JNIEnv* env,
                                                  jclass,
                                                  jboolean enable_validation) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_state && g_state->context.is_ready()) {
        OTGCAM_LOGW("nativeInit called but context already initialized");
        return env->NewStringUTF(g_state->context.device_summary().c_str());
    }
    auto st = std::make_unique<NativeState>();
    if (!st->context.create(enable_validation == JNI_TRUE)) {
        OTGCAM_LOGE("VulkanContext::create failed");
        return nullptr;
    }
    // Compute post-process pipeline (best effort: se la creazione fallisce
    // proseguiamo con il fallback CPU/blit).
    if (st->pp.create(st->context)) {
        st->compute_ready = true;
    } else {
        OTGCAM_LOGW("PostProcessPipeline::create failed; falling back to CPU YUYV path");
        st->compute_ready = false;
    }
    g_state = std::move(st);
    return env->NewStringUTF(g_state->context.device_summary().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeShutdown(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state) return;
    g_state->pp.destroy(g_state->context);
    g_state->uploader.destroy(g_state->context);
    g_state->renderer.detach_surface(g_state->context);
    if (g_state->window != nullptr) {
        ANativeWindow_release(g_state->window);
        g_state->window = nullptr;
    }
    g_state->context.destroy();
    g_state.reset();
    OTGCAM_LOGI("Native state destroyed");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeIsReady(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return (g_state && g_state->context.is_ready()) ? JNI_TRUE : JNI_FALSE;
}

// Acquisisce ANativeWindow dal Surface Java e (ri)crea la swapchain.
// Ritorna true se il renderer e' pronto a presentare frame.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeAttachSurface(JNIEnv* env,
                                                           jclass,
                                                           jobject surface) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state || !g_state->context.is_ready()) {
        OTGCAM_LOGE("nativeAttachSurface: VulkanContext not initialized");
        return JNI_FALSE;
    }
    if (surface == nullptr) {
        OTGCAM_LOGE("nativeAttachSurface: surface is null");
        return JNI_FALSE;
    }
    ANativeWindow* new_window = ANativeWindow_fromSurface(env, surface);
    if (new_window == nullptr) {
        OTGCAM_LOGE("ANativeWindow_fromSurface returned null");
        return JNI_FALSE;
    }
    // Sostituiamo la window precedente, rilasciandola.
    if (g_state->window != nullptr) {
        g_state->renderer.detach_surface(g_state->context);
        ANativeWindow_release(g_state->window);
        g_state->window = nullptr;
    }
    g_state->window = new_window;
    if (!g_state->renderer.attach_surface(g_state->context, new_window)) {
        OTGCAM_LOGE("PreviewRenderer::attach_surface failed");
        ANativeWindow_release(g_state->window);
        g_state->window = nullptr;
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeDetachSurface(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state) return;
    g_state->renderer.detach_surface(g_state->context);
    if (g_state->window != nullptr) {
        ANativeWindow_release(g_state->window);
        g_state->window = nullptr;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeRenderClear(JNIEnv*,
                                                         jclass,
                                                         jfloat r,
                                                         jfloat g,
                                                         jfloat b,
                                                         jfloat a) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state || !g_state->renderer.is_attached()) return JNI_FALSE;
    otgcam::ClearColor c{r, g, b, a};
    return g_state->renderer.render_clear(g_state->context, c) ? JNI_TRUE : JNI_FALSE;
}

// Persistenza della trasformazione user-space (scale + rotate + mirror).
// I valori saranno consumati dal compute shader in step 4+.
extern "C" JNIEXPORT void JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeSetTransform(JNIEnv*,
                                                          jclass,
                                                          jfloat scale_x,
                                                          jfloat scale_y,
                                                          jfloat rotate_deg,
                                                          jboolean mirror_x,
                                                          jboolean mirror_y) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state) return;
    g_state->scale_x    = scale_x;
    g_state->scale_y    = scale_y;
    g_state->rotate_deg = rotate_deg;
    g_state->mirror_x   = (mirror_x == JNI_TRUE);
    g_state->mirror_y   = (mirror_y == JNI_TRUE);
    OTGCAM_LOGD("setTransform scale=(%.3f,%.3f) rot=%.1f mirror=(%d,%d)",
                scale_x, scale_y, rotate_deg,
                g_state->mirror_x ? 1 : 0, g_state->mirror_y ? 1 : 0);
}

// Upload di un frame YUYV (YUY2) dalla camera UVC. La conversione
// YUYV->RGBA8 avviene CPU-side nel native; in step successivi sara'
// rimpiazzata da un compute shader.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeUploadFrameYUYV(JNIEnv* env,
                                                              jclass,
                                                              jobject buffer,
                                                              jint width,
                                                              jint height) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state || !g_state->context.is_ready()) return JNI_FALSE;
    if (buffer == nullptr || width <= 0 || height <= 0) return JNI_FALSE;
    void* addr = env->GetDirectBufferAddress(buffer);
    jlong cap  = env->GetDirectBufferCapacity(buffer);
    if (addr == nullptr || cap <= 0) {
        OTGCAM_LOGE("nativeUploadFrameYUYV: not a direct ByteBuffer");
        return JNI_FALSE;
    }
    bool ok;
    if (g_state->compute_ready) {
        // Path compute: copia YUYV nello staging dedicato (zero conversione CPU).
        ok = g_state->uploader.upload_yuyv_raw(g_state->context,
                                               static_cast<const uint8_t*>(addr),
                                               static_cast<size_t>(cap),
                                               static_cast<uint32_t>(width),
                                               static_cast<uint32_t>(height));
        if (ok) {
            // (Re)alloca la dst del compute alla risoluzione della swapchain.
            VkExtent2D ext = g_state->renderer.extent();
            if (ext.width > 0 && ext.height > 0) {
                if (!g_state->pp.ensure_dst(g_state->context, ext.width, ext.height)) {
                    ok = false;
                } else if (!g_state->pp.bind_inputs(g_state->context, g_state->uploader)) {
                    ok = false;
                }
            } else {
                // Swapchain non ancora pronta: rimanda il render al prossimo giro.
            }
        }
    } else {
        // Fallback CPU.
        ok = g_state->uploader.upload_yuyv(g_state->context,
                                           static_cast<const uint8_t*>(addr),
                                           static_cast<size_t>(cap),
                                           static_cast<uint32_t>(width),
                                           static_cast<uint32_t>(height));
    }
    if (ok) g_state->have_frame = true;
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Renderizza l'ultimo frame uploadato applicando aspect/rotation/mirror
// dallo state persistito da nativeSetTransform/nativeSetAspect.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeRenderFrame(JNIEnv*,
                                                         jclass) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state || !g_state->renderer.is_attached()) return JNI_FALSE;

    otgcam::PreviewRenderer::FrameTransform xf;
    xf.aspect   = g_state->aspect;
    xf.rotation = static_cast<int>(g_state->rotate_deg);
    xf.mirror_x = g_state->mirror_x;
    xf.mirror_y = g_state->mirror_y;
    otgcam::ClearColor bg{0.0f, 0.0f, 0.0f, 1.0f};

    if (!g_state->have_frame) {
        otgcam::ClearColor red{1.0f, 0.0f, 0.0f, 1.0f};
        return g_state->renderer.render_clear(g_state->context, red) ? JNI_TRUE : JNI_FALSE;
    }

    // Path compute (preferito).
    if (g_state->compute_ready &&
        g_state->uploader.yuyv_ready() &&
        g_state->pp.is_ready() &&
        g_state->pp.dst_width() > 0) {
        otgcam::PreviewRenderer::EnhancementParams enh;
        enh.sharpen = g_state->enh_sharpen;
        enh.denoise = g_state->enh_denoise;
        enh.defog   = g_state->enh_defog;
        enh.defog_enabled = g_state->enh_defog_enabled;
        bool ok = g_state->renderer.render_frame_compute(g_state->context,
                                                         g_state->uploader,
                                                         g_state->pp,
                                                         xf, enh, bg);
        return ok ? JNI_TRUE : JNI_FALSE;
    }

    // Fallback CPU path.
    if (!g_state->uploader.is_ready()) {
        otgcam::ClearColor red{1.0f, 0.0f, 0.0f, 1.0f};
        return g_state->renderer.render_clear(g_state->context, red) ? JNI_TRUE : JNI_FALSE;
    }
    bool ok = g_state->renderer.render_frame(g_state->context, g_state->uploader,
                                             xf, bg);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeSetAspect(JNIEnv*,
                                                       jclass,
                                                       jint aspect) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state) return;
    g_state->aspect = aspect;
}

// Aggiorna i parametri di enhancement (sharp/denoise/defog 0..100,
// defog_on bool). Cheap, no submit GPU: sara' applicato al prossimo
// render_frame_compute.
extern "C" JNIEXPORT void JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeSetEnhancement(JNIEnv*,
                                                            jclass,
                                                            jint sharpen_pct,
                                                            jint denoise_pct,
                                                            jint defog_pct,
                                                            jboolean defog_on) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state) return;
    auto clamp = [](int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); };
    g_state->enh_sharpen = clamp(sharpen_pct) / 100.0f;
    g_state->enh_denoise = clamp(denoise_pct) / 100.0f;
    g_state->enh_defog   = clamp(defog_pct)   / 100.0f;
    g_state->enh_defog_enabled = (defog_on == JNI_TRUE) ? 1u : 0u;
}
