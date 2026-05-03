#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace otgcam {

class VulkanContext;
class FrameUploader;

// PostProcessPipeline: compute shader che decodifica YUYV dal staging
// buffer di FrameUploader, downscala alla risoluzione della swapchain
// e applica denoise (bilateral 3x3) + sharpen (unsharp mask) + defog
// adattivo (modulato da luminanza media). Output: VkImage RGBA8 in
// TRANSFER_SRC_OPTIMAL pronta per il blit alla swapchain.
//
// Resource ownership:
//   - dst_image_ + memory: ricreata se cambia (dst_w, dst_h).
//   - shader_module_, pipeline_, layout_, descriptor pool: una sola volta.
//   - sampler_: nessuno (lavoriamo su buffer/image storage diretti).
class PostProcessPipeline {
public:
    PostProcessPipeline() = default;
    ~PostProcessPipeline();

    PostProcessPipeline(const PostProcessPipeline&) = delete;
    PostProcessPipeline& operator=(const PostProcessPipeline&) = delete;

    // Crea pipeline, layout, descriptor pool e set. Da chiamare una volta
    // dopo VulkanContext::create().
    bool create(VulkanContext& ctx);

    void destroy(VulkanContext& ctx);

    bool is_ready() const { return pipeline_ != VK_NULL_HANDLE; }

    // (Ri)alloca la dst image alla risoluzione richiesta. Idempotente
    // se size invariata. Lascia l'image in UNDEFINED.
    bool ensure_dst(VulkanContext& ctx, uint32_t w, uint32_t h);

    // Aggiorna i descriptor set con il buffer YUYV corrente di uploader.
    // Da richiamare se il buffer cambia (resize del frame in arrivo).
    bool bind_inputs(VulkanContext& ctx, FrameUploader& uploader);

    // Registra il dispatch del compute shader nel command buffer fornito.
    // Stato all'ingresso:
    //   - dst_image_ in qualsiasi layout (tipicamente TRANSFER_SRC_OPTIMAL
    //     dal frame precedente o UNDEFINED al primo uso): la barrier la
    //     porta in GENERAL.
    //   - yuyv buffer host-coherent gia' scritto da CPU.
    // Stato all'uscita: dst_image_ in TRANSFER_SRC_OPTIMAL.
    struct DispatchParams {
        uint32_t src_w;
        uint32_t src_h;
        float    sharpen;        // 0..1
        float    denoise;        // 0..1
        float    defog;          // 0..1
        uint32_t defog_enabled;  // 0/1
    };
    void record(VkCommandBuffer cb, const DispatchParams& p);

    VkImage  dst_image()  const { return dst_image_; }
    uint32_t dst_width()  const { return dst_w_; }
    uint32_t dst_height() const { return dst_h_; }

private:
    bool create_descriptor_set_layout(VkDevice dev);
    bool create_pipeline(VkDevice dev);
    bool create_descriptor_pool(VkDevice dev);

    bool create_dst_image(VulkanContext& ctx);
    void destroy_dst_image(VulkanContext& ctx);

    VkDescriptorSetLayout dsl_      = VK_NULL_HANDLE;
    VkPipelineLayout      layout_   = VK_NULL_HANDLE;
    VkShaderModule        module_   = VK_NULL_HANDLE;
    VkPipeline            pipeline_ = VK_NULL_HANDLE;

    VkDescriptorPool      desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set_  = VK_NULL_HANDLE;

    // Dst image (output del compute, input del blit alla swapchain).
    VkImage        dst_image_  = VK_NULL_HANDLE;
    VkDeviceMemory dst_memory_ = VK_NULL_HANDLE;
    VkImageView    dst_view_   = VK_NULL_HANDLE;
    uint32_t       dst_w_      = 0;
    uint32_t       dst_h_      = 0;
    VkImageLayout  dst_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

}  // namespace otgcam
