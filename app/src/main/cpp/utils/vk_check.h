#pragma once

#include <vulkan/vulkan.h>

#include "log.h"

// Restituisce una stringa human-readable per VkResult.
inline const char* vk_result_string(VkResult r) {
    switch (r) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_EVENT_SET:                      return "VK_EVENT_SET";
        case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
        case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        default:                                return "VK_UNKNOWN";
    }
}

// VK_CHECK: log + return su errore. Pensata per init code, non per hot path.
#define VK_CHECK(expr)                                                       \
    do {                                                                     \
        VkResult _vk_check_result = (expr);                                  \
        if (_vk_check_result != VK_SUCCESS) {                                \
            OTGCAM_LOGE("Vulkan call failed: %s -> %s (%d) at %s:%d",        \
                        #expr, vk_result_string(_vk_check_result),           \
                        (int)_vk_check_result, __FILE__, __LINE__);          \
            return false;                                                    \
        }                                                                    \
    } while (0)

// VK_CHECK_VOID: variante che ritorna void (per shutdown / cleanup).
#define VK_CHECK_VOID(expr)                                                  \
    do {                                                                     \
        VkResult _vk_check_result = (expr);                                  \
        if (_vk_check_result != VK_SUCCESS) {                                \
            OTGCAM_LOGW("Vulkan call non-fatal failure: %s -> %s (%d)",      \
                        #expr, vk_result_string(_vk_check_result),           \
                        (int)_vk_check_result);                              \
        }                                                                    \
    } while (0)
