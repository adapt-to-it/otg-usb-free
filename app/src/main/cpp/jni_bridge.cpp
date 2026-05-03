#include <jni.h>

#include "utils/log.h"

// Stub di partenza dello strato nativo. Le entry point JNI reali
// (init Vulkan, upload frame, ecc.) verranno aggiunte negli step successivi.
// In questo step verifichiamo solo che la toolchain (CMake + NDK + shaderc)
// produca un .so caricabile per i 4 ABI configurati.

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_otgcam_VulkanRenderer_nativeVersionString(JNIEnv* env, jclass) {
    OTGCAM_LOGI("nativeVersionString invoked");
    return env->NewStringUTF("otgcam_native v0.1 (step 1: toolchain)");
}
