#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace otgcam {

class VulkanContext;

// FrameUploader gestisce una texture intermedia RGBA8 + staging buffer
// host-visible usata per portare frame YUYV/RGBA dalla camera USB alla GPU.
//
// Path step 4 (CPU conversion):
//   - Java consegna un ByteBuffer YUYV/NV21/RGBA al JNI.
//   - upload_yuyv() / upload_rgba() converte (se serve) e copia nella
//     staging buffer + record di un command buffer che fa
//     buffer->image copy + barrier in SHADER_READ_ONLY_OPTIMAL.
//   - Il caller (PreviewRenderer) puo' fare vkCmdBlitImage da `image()`
//     alla swapchain image per il display.
//
// La texture si ricrea on-demand quando cambia size (resize handling).
// Non thread-safe: usare lo stesso mutex del JNI bridge.
class FrameUploader {
public:
    FrameUploader() = default;
    ~FrameUploader();

    FrameUploader(const FrameUploader&) = delete;
    FrameUploader& operator=(const FrameUploader&) = delete;

    // Crea/ricrea le risorse per (w, h) dopo aver atteso idle. Ritorna true
    // se la texture e' pronta.
    bool ensure_size(VulkanContext& ctx, uint32_t w, uint32_t h);

    // Upload da YUYV (YUY2): 4 byte per 2 pixel, ordine Y0 U Y1 V.
    // Esegue conversione CPU YUYV->RGBA8 nella staging buffer, poi
    // registra+submette un command buffer che copia in image_.
    // Bloccante (vkQueueWaitIdle dopo submit) per garantire che il blit
    // successivo veda i dati.
    bool upload_yuyv(VulkanContext& ctx,
                     const uint8_t* data,
                     size_t bytes,
                     uint32_t w,
                     uint32_t h);

    // Upload diretto di dati RGBA8 gia' in formato corretto.
    bool upload_rgba(VulkanContext& ctx,
                     const uint8_t* data,
                     size_t bytes,
                     uint32_t w,
                     uint32_t h);

    // Path compute: copia il buffer YUYV nello staging YUYV-only senza
    // alcuna conversione e senza copy-to-image. Ritorna true se ok; il
    // caller (PostProcessPipeline) legge poi il buffer dal compute shader.
    bool upload_yuyv_raw(VulkanContext& ctx,
                         const uint8_t* data,
                         size_t bytes,
                         uint32_t w,
                         uint32_t h);

    void destroy(VulkanContext& ctx);

    bool is_ready() const { return image_ != VK_NULL_HANDLE; }
    VkImage  image()  const { return image_; }
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    VkImageLayout current_layout() const { return current_layout_; }

    // Path compute: staging buffer YUYV-only, separato da quello RGBA.
    // Allocato/ricreato da upload_yuyv_raw(). Size in byte = w*h*2.
    VkBuffer     yuyv_buffer() const { return yuyv_buf_; }
    VkDeviceSize yuyv_size()   const { return yuyv_size_; }
    bool         yuyv_ready()  const { return yuyv_buf_ != VK_NULL_HANDLE; }

private:
    bool create_image(VulkanContext& ctx);
    bool create_staging(VulkanContext& ctx);
    bool create_cmd(VulkanContext& ctx);
    void destroy_inner(VulkanContext& ctx);

    bool submit_upload(VulkanContext& ctx);

    uint32_t width_  = 0;
    uint32_t height_ = 0;

    VkImage         image_         = VK_NULL_HANDLE;
    VkDeviceMemory  image_memory_  = VK_NULL_HANDLE;

    VkBuffer        staging_       = VK_NULL_HANDLE;
    VkDeviceMemory  staging_mem_   = VK_NULL_HANDLE;
    void*           staging_mapped_ = nullptr;
    VkDeviceSize    staging_size_  = 0;

    VkCommandPool   cmd_pool_      = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buf_       = VK_NULL_HANDLE;
    VkFence         upload_fence_  = VK_NULL_HANDLE;

    VkImageLayout   current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    // Staging YUYV-only (path compute). Indipendente dal pipeline RGBA.
    VkBuffer        yuyv_buf_       = VK_NULL_HANDLE;
    VkDeviceMemory  yuyv_mem_       = VK_NULL_HANDLE;
    void*           yuyv_mapped_    = nullptr;
    VkDeviceSize    yuyv_size_      = 0;
    uint32_t        yuyv_w_         = 0;
    uint32_t        yuyv_h_         = 0;
};

}  // namespace otgcam
