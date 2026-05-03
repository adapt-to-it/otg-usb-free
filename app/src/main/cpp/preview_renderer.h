#pragma once

#include <vulkan/vulkan.h>
#include <android/native_window.h>

#include <cstdint>

#include "vulkan_swapchain.h"

namespace otgcam {

class VulkanContext;

struct ClearColor {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

// PreviewRenderer guida il rendering del preview camera sulla swapchain.
//
// Step 3: render di un singolo clear-color (verifica swapchain end-to-end).
// Step 4+: aggiungeremo upload del frame, pipeline compute YUV->RGB, etc.
//
// Owner: il VulkanContext globale. Una sola istanza per processo, non
// thread-safe; l'accesso deve essere mediato dal mutex del JNI bridge.
class PreviewRenderer {
public:
    PreviewRenderer() = default;
    ~PreviewRenderer();

    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    bool attach_surface(VulkanContext& ctx, ANativeWindow* window);
    void detach_surface(VulkanContext& ctx);

    bool is_attached() const { return swapchain_.is_ready(); }

    // Renderizza un singolo frame con clear color e present.
    // Ritorna false su out-of-date / errore non recuperabile.
    bool render_clear(VulkanContext& ctx, const ClearColor& c);

    // Parametri di trasformazione applicati dal blit.
    // aspect: 0=fit (letterbox), 1=fill (crop), 2=stretch.
    // rotation: 0/90/180/270 (gradi, CCW dal punto di vista user).
    // mirror_x/y: flip dopo la rotation.
    struct FrameTransform {
        int  aspect = 0;
        int  rotation = 0;
        bool mirror_x = false;
        bool mirror_y = false;
    };

    // Renderizza il contenuto della texture intermedia (FrameUploader)
    // sulla swapchain image con vkCmdBlitImage. Calcola internamente il
    // rettangolo di destinazione in base ad aspect+rotation e flippa i
    // srcOffsets per mirror. Letterbox riempito con `bg`.
    bool render_frame(VulkanContext& ctx,
                      class FrameUploader& uploader,
                      const FrameTransform& xform,
                      const ClearColor& bg);

    VkExtent2D extent() const { return swapchain_.extent(); }

private:
    bool create_command_pool(VulkanContext& ctx);
    bool create_sync_objects(VulkanContext& ctx);
    void destroy_sync_objects(VulkanContext& ctx);
    void destroy_command_pool(VulkanContext& ctx);

    VulkanSwapchain swapchain_;
    VkCommandPool   cmd_pool_         = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buffer_       = VK_NULL_HANDLE;
    VkSemaphore     img_acquired_sem_ = VK_NULL_HANDLE;
    VkSemaphore     render_done_sem_  = VK_NULL_HANDLE;
    VkFence         in_flight_fence_  = VK_NULL_HANDLE;
};

}  // namespace otgcam
