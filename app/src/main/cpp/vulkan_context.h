#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace otgcam {

// VulkanContext incapsula instance + physical device + logical device + queue.
// E' il fondamento della pipeline; surface/swapchain/render verranno aggiunti
// negli step successivi.
//
// Una sola istanza globale per processo. Non thread-safe per la creazione/
// distruzione, thread-safe per l'accesso read-only ai membri dopo create().
class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Crea instance + device. Ritorna false se Vulkan non e' disponibile sul
    // device o se manca un'estensione/queue richiesta. In quel caso l'app
    // deve usare il fallback Surface (path UVCAndroid legacy).
    //
    // enable_validation: se true tenta di abilitare VK_LAYER_KHRONOS_validation
    // e VK_EXT_debug_utils. Se la layer non e' presente (release build, o
    // download fallito) l'init prosegue senza.
    bool create(bool enable_validation);

    // Distrugge tutto. Idempotente.
    void destroy();

    bool is_ready() const { return device_ != VK_NULL_HANDLE; }

    VkInstance       instance()       const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkDevice         device()         const { return device_; }
    VkQueue          queue()          const { return queue_; }
    uint32_t         queue_family_index() const { return queue_family_index_; }

    const VkPhysicalDeviceProperties& properties() const { return properties_; }

    // "<device_name>|api_<major>.<minor>.<patch>" - utile per UI/debug.
    std::string device_summary() const;

private:
    bool create_instance(bool enable_validation);
    bool setup_debug_messenger();
    bool pick_physical_device();
    bool create_logical_device();

    VkInstance               instance_            = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_     = VK_NULL_HANDLE;
    VkPhysicalDevice         physical_device_     = VK_NULL_HANDLE;
    VkDevice                 device_              = VK_NULL_HANDLE;
    VkQueue                  queue_               = VK_NULL_HANDLE;
    uint32_t                 queue_family_index_  = UINT32_MAX;
    bool                     validation_enabled_  = false;

    VkPhysicalDeviceProperties properties_{};
    std::vector<const char*>   enabled_instance_layers_;
    std::vector<const char*>   enabled_instance_extensions_;
    std::vector<const char*>   enabled_device_extensions_;
};

}  // namespace otgcam
