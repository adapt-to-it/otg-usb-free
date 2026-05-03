#include "compute_pipeline.h"

#include <shaderc/shaderc.hpp>

#include <cstring>
#include <vector>

#include "frame_uploader.h"
#include "utils/log.h"
#include "utils/vk_check.h"
#include "vulkan_context.h"

namespace otgcam {

namespace {

// Sorgente GLSL del compute shader.
const char* kPostProcessGlslSource = R"GLSL(
#version 450

layout (local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout (set = 0, binding = 0, std430) readonly buffer YuyvBuf {
    uint data[];
} yuyv;

layout (set = 0, binding = 1, rgba8) uniform writeonly image2D dst;

layout (push_constant) uniform Pc {
    uint  src_w;
    uint  src_h;
    uint  dst_w;
    uint  dst_h;
    float sharpen;
    float denoise;
    float defog;
    uint  defog_enabled;
} pc;

vec3 sample_yuyv(int sx, int sy) {
    sx = clamp(sx, 0, int(pc.src_w) - 1);
    sy = clamp(sy, 0, int(pc.src_h) - 1);
    uint pair_x = uint(sx) >> 1;
    uint words_per_row = pc.src_w >> 1;
    uint word_idx = uint(sy) * words_per_row + pair_x;
    uint w = yuyv.data[word_idx];
    int y0 = int( w        & 0xFFu);
    int u  = int((w >>  8) & 0xFFu);
    int y1 = int((w >> 16) & 0xFFu);
    int v  = int((w >> 24) & 0xFFu);
    int y = ((sx & 1) == 0) ? y0 : y1;
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int rr = (298 * c + 409 * e + 128) >> 8;
    int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int bb = (298 * c + 516 * d + 128) >> 8;
    return vec3(
        clamp(rr, 0, 255) / 255.0,
        clamp(gg, 0, 255) / 255.0,
        clamp(bb, 0, 255) / 255.0
    );
}

vec3 bilateral3x3(int cx, int cy, vec3 center, float strength) {
    if (strength <= 1e-3) return center;
    float sigma_r = mix(0.30, 0.06, strength);
    float lum_c = dot(center, vec3(0.299, 0.587, 0.114));
    vec3  acc = vec3(0.0);
    float w_sum = 0.0;
    float ws[9] = float[9](
        0.0625, 0.125, 0.0625,
        0.125,  0.25,  0.125,
        0.0625, 0.125, 0.0625
    );
    int idx = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec3 s = sample_yuyv(cx + dx, cy + dy);
            float lum_s = dot(s, vec3(0.299, 0.587, 0.114));
            float wr    = exp(-pow(lum_s - lum_c, 2.0) / (2.0 * sigma_r * sigma_r));
            float w     = ws[idx] * wr;
            acc   += s * w;
            w_sum += w;
            ++idx;
        }
    }
    vec3 filt = acc / max(w_sum, 1e-6);
    return mix(center, filt, strength);
}

vec3 blur5x5(int cx, int cy) {
    // Blur 5x5 (raggio 2): separa meglio le frequenze rispetto al 3x3,
    // necessario perche' il downscale 4K->2K ha gia' "rimosso" le alte freq
    // contigue. Senza questo, c - blur e' quasi zero a 4 pixel di distanza.
    vec3 acc = vec3(0.0);
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            acc += sample_yuyv(cx + dx, cy + dy);
        }
    }
    return acc * (1.0 / 25.0);
}

// Statistiche locali 5x5: min(R,G,B) globale + luminanza media.
// Single-pass, no atomics: l'unico modo di stimare "fog density" senza
// reduction. min_rgb e' il "dark channel locale" (proxy della velatura).
struct LocalStats {
    float min_rgb;  // proxy fog density
    float l_mean;   // luma media -> usata per preservare la luminosita'
};

LocalStats local_stats5x5(int cx, int cy) {
    LocalStats s;
    s.min_rgb = 1.0;
    float lsum = 0.0;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            vec3 v = sample_yuyv(cx + dx, cy + dy);
            s.min_rgb = min(s.min_rgb, min(v.r, min(v.g, v.b)));
            lsum += dot(v, vec3(0.299, 0.587, 0.114));
        }
    }
    s.l_mean = lsum / 25.0;
    return s;
}

void main() {
    uvec2 gid = gl_GlobalInvocationID.xy;
    if (gid.x >= pc.dst_w || gid.y >= pc.dst_h) return;
    float fx = (float(gid.x) + 0.5) * float(pc.src_w) / float(pc.dst_w);
    float fy = (float(gid.y) + 0.5) * float(pc.src_h) / float(pc.dst_h);
    int sx = int(fx);
    int sy = int(fy);

    vec3 c = sample_yuyv(sx, sy);
    c = bilateral3x3(sx, sy, c, pc.denoise);
    if (pc.sharpen > 1e-3) {
        // Unsharp mask con blur 5x5 (raggio 2) e fattore 6x: necessario
        // perche' lavoriamo dopo il downscale 4K->dst, dove le alte frequenze
        // tra pixel adiacenti sono gia' attenuate.
        vec3 b = blur5x5(sx, sy);
        vec3 high = c - b;
        c = clamp(c + high * pc.sharpen * 6.0, 0.0, 1.0);
    }
    if (pc.defog > 1e-3) {
        // Defog mean-preserving (no scurimento globale).
        //
        // Strategia (da survey di tecniche real-time GPU dehaze):
        //   1. Stima fog locale via min(R,G,B) su 5x5 (dark channel locale).
        //   2. Sottrazione modulata in YCbCr (solo Y), evita hue shift.
        //   3. Local contrast stretch attorno a l_mean 5x5 (NON a 0.5)
        //      -> stretch e' mean-preserving per costruzione.
        //   4. Boost saturazione tramite scaling cromatico, modesto.
        //   5. Re-normalizzazione finale per riportare la Y media del
        //      pixel alla l_mean originale (soft, modulata da t).
        // Niente black-point lift fisso, niente sottrazioni costanti,
        // niente operazioni RGB per-canale che generano dominanti.
        float L = dot(c, vec3(0.299, 0.587, 0.114));
        float t = (pc.defog_enabled != 0u)
            ? (smoothstep(0.45, 0.85, L) * pc.defog)
            : pc.defog;
        if (t > 1e-3) {
            LocalStats st = local_stats5x5(sx, sy);
            // Soglia "ombre": sotto a questo valore, le operazioni di
            // rescaling amplificano il rumore di crominanza YUYV creando
            // alone viola/verde. Le bypassiamo per le zone scure.
            float shadow_w = smoothstep(0.08, 0.25, L);
            float t_eff = t * shadow_w;
            if (t_eff <= 1e-3) {
                // Pixel troppo scuro: lascialo intatto.
            } else {
                // Step 1+3: contrast stretch attorno a l_mean locale.
                // Coefficiente alzato a 0.9 (era 0.35): stretch piu' deciso,
                // mantenendo per costruzione l'invarianza del mean (l_mean
                // e' il punto fisso).
                float k_ct = 1.0 + t_eff * 0.9;
                float Y_stretched = (L - st.l_mean) * k_ct + st.l_mean;
                // Step 2: sottrazione fog (dark-channel locale). 0.4
                // permette di vedere l'effetto "togli velatura" quando lo
                // slider e' al max, attenuata da min_rgb (zone scure intatte).
                float Y_dehazed = Y_stretched - 0.4 * t_eff * st.min_rgb;
                Y_dehazed = clamp(Y_dehazed, 0.0, 1.0);
                // Step 5: rinormalizza per recuperare la luminanza media.
                // Gain clamp piu' largo (0.75..1.9) e peso 0.65 nel mix.
                float gain = (Y_dehazed > 1e-3)
                    ? mix(1.0, st.l_mean / Y_dehazed, 0.65 * t_eff)
                    : 1.0;
                gain = clamp(gain, 0.75, 1.9);
                float Y_final = clamp(Y_dehazed * gain, 0.0, 1.0);
                // Rescale RGB per Y_final/L: preserva crominanza esatta.
                float ratio = clamp(Y_final / max(L, 0.01), 0.6, 2.2);
                vec3 dehazed = c * ratio;
                // Step 4: saturazione boost piu' visibile (0.6 a max),
                // modulata da L per non amplificare chroma noise nelle ombre.
                float Lc = dot(dehazed, vec3(0.299, 0.587, 0.114));
                float sat_boost = 1.0 + t_eff * 0.6 * smoothstep(0.15, 0.5, L);
                vec3 sat = mix(vec3(Lc), dehazed, sat_boost);
                sat = clamp(sat, 0.0, 1.0);
                c = mix(c, sat, t_eff);
            }
        }
    }
    imageStore(dst, ivec2(gid), vec4(c, 1.0));
}
)GLSL";

uint32_t find_memory_type(VkPhysicalDevice phys,
                          uint32_t type_bits,
                          VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool compile_glsl(const char* source, const char* name,
                  std::vector<uint32_t>& spv_out) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions opt;
    opt.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    opt.SetOptimizationLevel(shaderc_optimization_level_performance);
    auto res = compiler.CompileGlslToSpv(source, strlen(source),
                                         shaderc_compute_shader,
                                         name, "main", opt);
    if (res.GetCompilationStatus() != shaderc_compilation_status_success) {
        OTGCAM_LOGE("shaderc compile '%s' failed: %s", name,
                    res.GetErrorMessage().c_str());
        return false;
    }
    spv_out.assign(res.cbegin(), res.cend());
    return true;
}

void image_barrier(VkCommandBuffer cb, VkImage img,
                   VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_acc, VkAccessFlags dst_acc,
                   VkPipelineStageFlags src_st, VkPipelineStageFlags dst_st) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_acc;
    b.dstAccessMask = dst_acc;
    vkCmdPipelineBarrier(cb, src_st, dst_st, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

PostProcessPipeline::~PostProcessPipeline() {
    if (pipeline_ != VK_NULL_HANDLE || dst_image_ != VK_NULL_HANDLE) {
        OTGCAM_LOGW("PostProcessPipeline destructor: destroy() not called");
    }
}

bool PostProcessPipeline::create(VulkanContext& ctx) {
    VkDevice dev = ctx.device();
    if (!create_descriptor_set_layout(dev)) return false;
    if (!create_pipeline(dev))              return false;
    if (!create_descriptor_pool(dev))       return false;
    OTGCAM_LOGI("PostProcessPipeline ready");
    return true;
}

void PostProcessPipeline::destroy(VulkanContext& ctx) {
    VkDevice dev = ctx.device();
    if (dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(dev);
    }
    destroy_dst_image(ctx);
    if (desc_pool_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, desc_pool_, nullptr);
    }
    desc_pool_ = VK_NULL_HANDLE;
    desc_set_  = VK_NULL_HANDLE;
    if (pipeline_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    if (module_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, module_, nullptr);
    }
    module_ = VK_NULL_HANDLE;
    if (layout_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, layout_, nullptr);
    }
    layout_ = VK_NULL_HANDLE;
    if (dsl_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, dsl_, nullptr);
    }
    dsl_ = VK_NULL_HANDLE;
}

bool PostProcessPipeline::create_descriptor_set_layout(VkDevice dev) {
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 2;
    ci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &dsl_));
    return true;
}

bool PostProcessPipeline::create_pipeline(VkDevice dev) {
    std::vector<uint32_t> spv;
    if (!compile_glsl(kPostProcessGlslSource, "postprocess.comp", spv)) {
        return false;
    }
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spv.size() * sizeof(uint32_t);
    smci.pCode = spv.data();
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &module_));

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(DispatchParams) + sizeof(uint32_t) * 2;  // include dst_w/dst_h
    // In realta' pushiamo: src_w, src_h, dst_w, dst_h, sharpen, denoise, defog, defog_enabled
    // = 4 uint + 3 float + 1 uint = 32 byte.
    pcr.size = 32;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &layout_));

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module_;
    cpci.stage.pName = "main";
    cpci.layout = layout_;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline_));
    return true;
}

bool PostProcessPipeline::create_descriptor_pool(VkDevice dev) {
    VkDescriptorPoolSize sz[2]{};
    sz[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sz[0].descriptorCount = 1;
    sz[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sz[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = 1;
    ci.poolSizeCount = 2;
    ci.pPoolSizes = sz;
    VK_CHECK(vkCreateDescriptorPool(dev, &ci, nullptr, &desc_pool_));

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = desc_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsl_;
    VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &desc_set_));
    return true;
}

bool PostProcessPipeline::ensure_dst(VulkanContext& ctx, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return false;
    if (dst_image_ != VK_NULL_HANDLE && w == dst_w_ && h == dst_h_) return true;
    destroy_dst_image(ctx);
    dst_w_ = w;
    dst_h_ = h;
    if (!create_dst_image(ctx)) {
        destroy_dst_image(ctx);
        return false;
    }
    return true;
}

bool PostProcessPipeline::create_dst_image(VulkanContext& ctx) {
    VkDevice dev = ctx.device();
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {dst_w_, dst_h_, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ici, nullptr, &dst_image_));

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev, dst_image_, &mr);
    uint32_t mt = find_memory_type(ctx.physical_device(), mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        OTGCAM_LOGE("compute dst: no DEVICE_LOCAL memory type");
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &dst_memory_));
    VK_CHECK(vkBindImageMemory(dev, dst_image_, dst_memory_, 0));

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = dst_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vci, nullptr, &dst_view_));
    dst_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void PostProcessPipeline::destroy_dst_image(VulkanContext& ctx) {
    VkDevice dev = ctx.device();
    if (dst_view_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, dst_view_, nullptr);
    }
    dst_view_ = VK_NULL_HANDLE;
    if (dst_image_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyImage(dev, dst_image_, nullptr);
    }
    dst_image_ = VK_NULL_HANDLE;
    if (dst_memory_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkFreeMemory(dev, dst_memory_, nullptr);
    }
    dst_memory_ = VK_NULL_HANDLE;
    dst_w_ = 0;
    dst_h_ = 0;
    dst_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

bool PostProcessPipeline::bind_inputs(VulkanContext& ctx, FrameUploader& uploader) {
    if (!uploader.yuyv_ready() || dst_view_ == VK_NULL_HANDLE) return false;
    VkDescriptorBufferInfo bi{};
    bi.buffer = uploader.yuyv_buffer();
    bi.offset = 0;
    bi.range = uploader.yuyv_size();

    VkDescriptorImageInfo ii{};
    ii.imageView = dst_view_;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = desc_set_;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[0].pBufferInfo = &bi;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = desc_set_;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(ctx.device(), 2, w, 0, nullptr);
    return true;
}

void PostProcessPipeline::record(VkCommandBuffer cb, const DispatchParams& p) {
    if (pipeline_ == VK_NULL_HANDLE || dst_image_ == VK_NULL_HANDLE) return;

    // dst_image_ -> GENERAL per imageStore.
    image_barrier(cb, dst_image_,
                  dst_layout_, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &desc_set_, 0, nullptr);

    // Push constants: stesso layout del block GLSL.
    struct {
        uint32_t src_w, src_h, dst_w, dst_h;
        float    sharpen, denoise, defog;
        uint32_t defog_enabled;
    } pc;
    pc.src_w = p.src_w;
    pc.src_h = p.src_h;
    pc.dst_w = dst_w_;
    pc.dst_h = dst_h_;
    pc.sharpen = p.sharpen;
    pc.denoise = p.denoise;
    pc.defog   = p.defog;
    pc.defog_enabled = p.defog_enabled;
    vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    uint32_t gx = (dst_w_ + 15) / 16;
    uint32_t gy = (dst_h_ + 15) / 16;
    vkCmdDispatch(cb, gx, gy, 1);

    // dst_image_ -> TRANSFER_SRC_OPTIMAL per il blit successivo.
    image_barrier(cb, dst_image_,
                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    dst_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

}  // namespace otgcam
