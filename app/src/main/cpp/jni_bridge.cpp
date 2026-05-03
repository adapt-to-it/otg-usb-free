#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include "frame_uploader.h"
#include "preview_renderer.h"
#include "utils/log.h"
#include "vulkan_context.h"

namespace {

// Stato globale del modulo nativo. Una sola istanza per processo.
struct NativeState {
    otgcam::VulkanContext   context;
    otgcam::PreviewRenderer renderer;
    otgcam::FrameUploader   uploader;
    ANativeWindow*          window = nullptr;
    bool                    have_frame = false;

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
    g_state = std::move(st);
    return env->NewStringUTF(g_state->context.device_summary().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeShutdown(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state) return;
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
    bool ok = g_state->uploader.upload_yuyv(g_state->context,
                                            static_cast<const uint8_t*>(addr),
                                            static_cast<size_t>(cap),
                                            static_cast<uint32_t>(width),
                                            static_cast<uint32_t>(height));
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
    if (!g_state->have_frame || !g_state->uploader.is_ready()) {
        otgcam::ClearColor red{1.0f, 0.0f, 0.0f, 1.0f};
        return g_state->renderer.render_clear(g_state->context, red) ? JNI_TRUE : JNI_FALSE;
    }
    otgcam::PreviewRenderer::FrameTransform xf;
    xf.aspect   = g_state->aspect;
    xf.rotation = static_cast<int>(g_state->rotate_deg);
    xf.mirror_x = g_state->mirror_x;
    xf.mirror_y = g_state->mirror_y;
    otgcam::ClearColor bg{0.0f, 0.0f, 0.0f, 1.0f};
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
