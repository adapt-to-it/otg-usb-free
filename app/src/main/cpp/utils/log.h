#pragma once

#include <android/log.h>

#define OTGCAM_LOG_TAG "OtgCamNative"

#define OTGCAM_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  OTGCAM_LOG_TAG, __VA_ARGS__)
#define OTGCAM_LOGW(...) __android_log_print(ANDROID_LOG_WARN,  OTGCAM_LOG_TAG, __VA_ARGS__)
#define OTGCAM_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, OTGCAM_LOG_TAG, __VA_ARGS__)

#if defined(OTGCAM_DEBUG)
#define OTGCAM_LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, OTGCAM_LOG_TAG, __VA_ARGS__)
#else
#define OTGCAM_LOGD(...) ((void)0)
#endif
