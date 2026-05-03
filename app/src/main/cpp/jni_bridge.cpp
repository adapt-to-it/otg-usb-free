#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "utils/log.h"
#include "vulkan_context.h"

namespace {

// Singleton del context Vulkan. Possediamo un unique_ptr globale: e' OK perche'
// l'app intera ha una sola pipeline Vulkan attiva.
std::mutex g_context_mutex;
std::unique_ptr<otgcam::VulkanContext> g_context;

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeVersionString(JNIEnv* env, jclass) {
    return env->NewStringUTF("otgcam_native v0.2 (step 2: vulkan context)");
}

// Inizializza il VulkanContext. Ritorna una stringa "device_name|api_x.y.z|..."
// in caso di successo, oppure null se Vulkan non e' utilizzabile sul device
// (l'app deve cadere sul path Surface della libreria UVCAndroid).
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeInit(JNIEnv* env,
                                                  jclass,
                                                  jboolean enable_validation) {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    if (g_context && g_context->is_ready()) {
        OTGCAM_LOGW("nativeInit called but context already initialized");
        return env->NewStringUTF(g_context->device_summary().c_str());
    }
    auto ctx = std::make_unique<otgcam::VulkanContext>();
    if (!ctx->create(enable_validation == JNI_TRUE)) {
        OTGCAM_LOGE("VulkanContext::create failed");
        return nullptr;
    }
    g_context = std::move(ctx);
    return env->NewStringUTF(g_context->device_summary().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeShutdown(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    if (!g_context) return;
    g_context->destroy();
    g_context.reset();
    OTGCAM_LOGI("Vulkan context destroyed");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeIsReady(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    return (g_context && g_context->is_ready()) ? JNI_TRUE : JNI_FALSE;
}
