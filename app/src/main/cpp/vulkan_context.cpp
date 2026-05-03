#include "vulkan_context.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

#include "utils/log.h"
#include "utils/vk_check.h"

namespace otgcam {

namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

// Estensioni instance richieste in tutti i build.
// VK_KHR_surface + VK_KHR_android_surface sono necessarie per creare la
// VkSurfaceKHR dal SurfaceView (step 3). Le abilitiamo gia' qui per evitare
// di ricreare l'instance piu' avanti.
const std::array<const char*, 2> kRequiredInstanceExtensions = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    "VK_KHR_android_surface",
};

const std::array<const char*, 1> kRequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user_data*/) {
    if (data == nullptr || data->pMessage == nullptr) return VK_FALSE;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        OTGCAM_LOGE("[VkValidation] %s", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        OTGCAM_LOGW("[VkValidation] %s", data->pMessage);
    } else {
        OTGCAM_LOGD("[VkValidation] %s", data->pMessage);
    }
    return VK_FALSE;
}

bool is_layer_available(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

bool is_instance_extension_available(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> exts(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data()) != VK_SUCCESS) return false;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

bool device_supports_extensions(VkPhysicalDevice dev,
                                const std::vector<const char*>& required) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
    for (const char* req : required) {
        bool found = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, req) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

uint32_t pick_graphics_compute_queue_family(VkPhysicalDevice dev) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());
    constexpr VkQueueFlags kRequired = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & kRequired) == kRequired) return i;
    }
    return UINT32_MAX;
}

}  // namespace

VulkanContext::~VulkanContext() {
    destroy();
}

bool VulkanContext::create(bool enable_validation) {
    if (instance_ != VK_NULL_HANDLE) {
        OTGCAM_LOGW("VulkanContext::create called twice; ignoring");
        return true;
    }
    if (!create_instance(enable_validation)) {
        destroy();
        return false;
    }
    if (validation_enabled_ && !setup_debug_messenger()) {
        // Non fatale: prosegui senza messenger.
        OTGCAM_LOGW("Debug messenger setup failed, continuing without it");
    }
    if (!pick_physical_device()) {
        destroy();
        return false;
    }
    if (!create_logical_device()) {
        destroy();
        return false;
    }
    OTGCAM_LOGI("Vulkan context ready: %s", device_summary().c_str());
    return true;
}

void VulkanContext::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        queue_family_index_ = UINT32_MAX;
    }
    if (debug_messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        auto destroy_fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_fn != nullptr) {
            destroy_fn(instance_, debug_messenger_, nullptr);
        }
        debug_messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physical_device_ = VK_NULL_HANDLE;
    enabled_instance_layers_.clear();
    enabled_instance_extensions_.clear();
    enabled_device_extensions_.clear();
    validation_enabled_ = false;
}

bool VulkanContext::create_instance(bool enable_validation) {
    enabled_instance_extensions_.clear();
    for (const char* ext : kRequiredInstanceExtensions) {
        if (!is_instance_extension_available(ext)) {
            OTGCAM_LOGE("Required instance extension missing: %s", ext);
            return false;
        }
        enabled_instance_extensions_.push_back(ext);
    }

    enabled_instance_layers_.clear();
    validation_enabled_ = false;
    if (enable_validation) {
        if (is_layer_available(kValidationLayerName)) {
            enabled_instance_layers_.push_back(kValidationLayerName);
            if (is_instance_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                enabled_instance_extensions_.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                validation_enabled_ = true;
                OTGCAM_LOGI("Vulkan validation layers enabled");
            } else {
                OTGCAM_LOGW("VK_EXT_debug_utils unavailable; validation messages will not be logged");
            }
        } else {
            OTGCAM_LOGI("Validation layer %s not present (release build or layers not bundled)",
                        kValidationLayerName);
        }
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "OtgUsbFree";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "otgcam_native";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app_info;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabled_instance_extensions_.size());
    ci.ppEnabledExtensionNames = enabled_instance_extensions_.data();
    ci.enabledLayerCount = static_cast<uint32_t>(enabled_instance_layers_.size());
    ci.ppEnabledLayerNames = enabled_instance_layers_.data();

    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        OTGCAM_LOGE("vkCreateInstance failed: %s (%d)", vk_result_string(r), (int)r);
        return false;
    }
    return true;
}

bool VulkanContext::setup_debug_messenger() {
    auto create_fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (create_fn == nullptr) {
        OTGCAM_LOGW("vkCreateDebugUtilsMessengerEXT not available");
        return false;
    }
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debug_callback;
    VK_CHECK(create_fn(instance_, &ci, nullptr, &debug_messenger_));
    return true;
}

bool VulkanContext::pick_physical_device() {
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        OTGCAM_LOGE("No Vulkan physical devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));

    // Su Android di norma c'e' un solo device. Scegliamo il primo che abbia
    // graphics+compute e supporti VK_KHR_swapchain.
    std::vector<const char*> required(kRequiredDeviceExtensions.begin(),
                                      kRequiredDeviceExtensions.end());
    for (VkPhysicalDevice dev : devices) {
        if (!device_supports_extensions(dev, required)) continue;
        uint32_t qf = pick_graphics_compute_queue_family(dev);
        if (qf == UINT32_MAX) continue;
        physical_device_ = dev;
        queue_family_index_ = qf;
        vkGetPhysicalDeviceProperties(physical_device_, &properties_);
        return true;
    }
    OTGCAM_LOGE("No suitable Vulkan physical device (need graphics+compute and swapchain)");
    return false;
}

bool VulkanContext::create_logical_device() {
    enabled_device_extensions_.assign(kRequiredDeviceExtensions.begin(),
                                      kRequiredDeviceExtensions.end());

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family_index_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures features{};
    // Per ora nessuna feature opzionale richiesta. Aggiungere qui se serve
    // (samplerAnisotropy, shaderInt16, ecc.) negli step successivi.

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(enabled_device_extensions_.size());
    dci.ppEnabledExtensionNames = enabled_device_extensions_.data();
    dci.pEnabledFeatures = &features;

    VK_CHECK(vkCreateDevice(physical_device_, &dci, nullptr, &device_));
    vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
    return true;
}

std::string VulkanContext::device_summary() const {
    std::ostringstream oss;
    oss << (properties_.deviceName[0] ? properties_.deviceName : "<unknown>")
        << "|api_"
        << VK_VERSION_MAJOR(properties_.apiVersion) << "."
        << VK_VERSION_MINOR(properties_.apiVersion) << "."
        << VK_VERSION_PATCH(properties_.apiVersion)
        << "|qf_" << queue_family_index_
        << "|validation_" << (validation_enabled_ ? "on" : "off");
    return oss.str();
}

}  // namespace otgcam
