#include "frame_uploader.h"

#include <algorithm>
#include <cstring>

#include "utils/log.h"
#include "utils/vk_check.h"
#include "vulkan_context.h"

namespace otgcam {

namespace {

constexpr VkFormat kImageFormat = VK_FORMAT_R8G8B8A8_UNORM;

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

// Conversione YUYV (YUY2) -> RGBA8.
// YUYV: 4 byte per 2 pixel, ordine [Y0, U, Y1, V].
// Formula BT.601 limited range, output full-range RGBA8.
inline void yuv_to_rgb(int y, int u, int v,
                       uint8_t& r, uint8_t& g, uint8_t& b) {
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int rr = (298 * c + 409 * e + 128) >> 8;
    int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int bb = (298 * c + 516 * d + 128) >> 8;
    r = static_cast<uint8_t>(std::clamp(rr, 0, 255));
    g = static_cast<uint8_t>(std::clamp(gg, 0, 255));
    b = static_cast<uint8_t>(std::clamp(bb, 0, 255));
}

void convert_yuyv_to_rgba(const uint8_t* in, uint8_t* out,
                          uint32_t w, uint32_t h) {
    const size_t pairs = static_cast<size_t>(w) * h / 2;
    for (size_t i = 0; i < pairs; ++i) {
        int y0 = in[0];
        int u  = in[1];
        int y1 = in[2];
        int v  = in[3];
        uint8_t r, g, b;
        yuv_to_rgb(y0, u, v, r, g, b);
        out[0] = r; out[1] = g; out[2] = b; out[3] = 255;
        yuv_to_rgb(y1, u, v, r, g, b);
        out[4] = r; out[5] = g; out[6] = b; out[7] = 255;
        in  += 4;
        out += 8;
    }
}

void barrier(VkCommandBuffer cb, VkImage img,
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

FrameUploader::~FrameUploader() {
    if (image_ != VK_NULL_HANDLE) {
        OTGCAM_LOGW("FrameUploader destructor: destroy() not called");
    }
}

bool FrameUploader::ensure_size(VulkanContext& ctx, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return false;
    if (image_ != VK_NULL_HANDLE && w == width_ && h == height_) {
        return true;
    }
    if (image_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx.device());
        destroy_inner(ctx);
    }
    width_  = w;
    height_ = h;
    if (!create_image(ctx))   { destroy_inner(ctx); return false; }
    if (!create_staging(ctx)) { destroy_inner(ctx); return false; }
    if (!create_cmd(ctx))     { destroy_inner(ctx); return false; }
    OTGCAM_LOGI("FrameUploader ready: %ux%u (RGBA8)", width_, height_);
    return true;
}

void FrameUploader::destroy(VulkanContext& ctx) {
    if (ctx.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx.device());
    }
    destroy_inner(ctx);
}

bool FrameUploader::upload_yuyv_raw(VulkanContext& ctx,
                                    const uint8_t* data, size_t bytes,
                                    uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || data == nullptr) return false;
    const VkDeviceSize need = static_cast<VkDeviceSize>(w) * h * 2;
    if (bytes < need) {
        OTGCAM_LOGE("upload_yuyv_raw: bytes=%zu expected>=%llu", bytes,
                    static_cast<unsigned long long>(need));
        return false;
    }
    VkDevice dev = ctx.device();
    if (yuyv_buf_ != VK_NULL_HANDLE && (yuyv_w_ != w || yuyv_h_ != h)) {
        // Resize: distrugge e ricrea.
        vkDeviceWaitIdle(dev);
        if (yuyv_mapped_) { vkUnmapMemory(dev, yuyv_mem_); yuyv_mapped_ = nullptr; }
        if (yuyv_buf_)    { vkDestroyBuffer(dev, yuyv_buf_, nullptr); yuyv_buf_ = VK_NULL_HANDLE; }
        if (yuyv_mem_)    { vkFreeMemory(dev, yuyv_mem_, nullptr); yuyv_mem_ = VK_NULL_HANDLE; }
        yuyv_size_ = 0;
    }
    if (yuyv_buf_ == VK_NULL_HANDLE) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = need;
        // STORAGE_BUFFER per accesso compute, TRANSFER_SRC come backup.
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &yuyv_buf_));
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(dev, yuyv_buf_, &mr);
        uint32_t mt = find_memory_type(ctx.physical_device(), mr.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX) {
            OTGCAM_LOGE("upload_yuyv_raw: no HOST_VISIBLE memory type");
            return false;
        }
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mt;
        VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &yuyv_mem_));
        VK_CHECK(vkBindBufferMemory(dev, yuyv_buf_, yuyv_mem_, 0));
        VK_CHECK(vkMapMemory(dev, yuyv_mem_, 0, mr.size, 0, &yuyv_mapped_));
        yuyv_size_ = need;
        yuyv_w_ = w;
        yuyv_h_ = h;
        OTGCAM_LOGI("FrameUploader YUYV staging ready: %ux%u (%llu bytes)",
                    w, h, static_cast<unsigned long long>(need));
    }
    std::memcpy(yuyv_mapped_, data, need);
    width_ = w;
    height_ = h;
    return true;
}

void FrameUploader::destroy_inner(VulkanContext& ctx) {
    VkDevice dev = ctx.device();
    if (yuyv_mapped_ && dev != VK_NULL_HANDLE) {
        vkUnmapMemory(dev, yuyv_mem_);
    }
    yuyv_mapped_ = nullptr;
    if (yuyv_buf_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, yuyv_buf_, nullptr);
    }
    yuyv_buf_ = VK_NULL_HANDLE;
    if (yuyv_mem_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkFreeMemory(dev, yuyv_mem_, nullptr);
    }
    yuyv_mem_  = VK_NULL_HANDLE;
    yuyv_size_ = 0;
    yuyv_w_    = 0;
    yuyv_h_    = 0;
    if (upload_fence_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyFence(dev, upload_fence_, nullptr);
    }
    upload_fence_ = VK_NULL_HANDLE;
    if (cmd_pool_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, cmd_pool_, nullptr);
    }
    cmd_pool_ = VK_NULL_HANDLE;
    cmd_buf_  = VK_NULL_HANDLE;
    if (staging_mapped_ != nullptr && dev != VK_NULL_HANDLE) {
        vkUnmapMemory(dev, staging_mem_);
    }
    staging_mapped_ = nullptr;
    if (staging_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, staging_, nullptr);
    }
    staging_ = VK_NULL_HANDLE;
    if (staging_mem_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkFreeMemory(dev, staging_mem_, nullptr);
    }
    staging_mem_  = VK_NULL_HANDLE;
    staging_size_ = 0;
    if (image_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyImage(dev, image_, nullptr);
    }
    image_ = VK_NULL_HANDLE;
    if (image_memory_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkFreeMemory(dev, image_memory_, nullptr);
    }
    image_memory_   = VK_NULL_HANDLE;
    current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

bool FrameUploader::create_image(VulkanContext& ctx) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kImageFormat;
    ici.extent = {width_, height_, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &image_));

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(ctx.device(), image_, &mr);
    uint32_t mt = find_memory_type(ctx.physical_device(),
                                   mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        OTGCAM_LOGE("FrameUploader: no DEVICE_LOCAL memory type");
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    VK_CHECK(vkAllocateMemory(ctx.device(), &ai, nullptr, &image_memory_));
    VK_CHECK(vkBindImageMemory(ctx.device(), image_, image_memory_, 0));
    current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

bool FrameUploader::create_staging(VulkanContext& ctx) {
    staging_size_ = static_cast<VkDeviceSize>(width_) * height_ * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = staging_size_;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(ctx.device(), &bci, nullptr, &staging_));

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(ctx.device(), staging_, &mr);
    uint32_t mt = find_memory_type(ctx.physical_device(),
                                   mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        OTGCAM_LOGE("FrameUploader: no HOST_VISIBLE memory type");
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    VK_CHECK(vkAllocateMemory(ctx.device(), &ai, nullptr, &staging_mem_));
    VK_CHECK(vkBindBufferMemory(ctx.device(), staging_, staging_mem_, 0));
    VK_CHECK(vkMapMemory(ctx.device(), staging_mem_, 0, mr.size, 0,
                         &staging_mapped_));
    return true;
}

bool FrameUploader::create_cmd(VulkanContext& ctx) {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.queue_family_index();
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pci, nullptr, &cmd_pool_));

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = cmd_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cai, &cmd_buf_));

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_CHECK(vkCreateFence(ctx.device(), &fi, nullptr, &upload_fence_));
    return true;
}

bool FrameUploader::upload_yuyv(VulkanContext& ctx,
                                const uint8_t* data, size_t bytes,
                                uint32_t w, uint32_t h) {
    if (!ensure_size(ctx, w, h)) return false;
    const size_t expected = static_cast<size_t>(w) * h * 2;
    if (bytes < expected) {
        OTGCAM_LOGE("upload_yuyv: bytes=%zu expected>=%zu (%ux%u)", bytes, expected, w, h);
        return false;
    }
    convert_yuyv_to_rgba(data,
                         static_cast<uint8_t*>(staging_mapped_), w, h);
    return submit_upload(ctx);
}

bool FrameUploader::upload_rgba(VulkanContext& ctx,
                                const uint8_t* data, size_t bytes,
                                uint32_t w, uint32_t h) {
    if (!ensure_size(ctx, w, h)) return false;
    const size_t expected = static_cast<size_t>(w) * h * 4;
    if (bytes < expected) {
        OTGCAM_LOGE("upload_rgba: bytes=%zu expected>=%zu (%ux%u)", bytes, expected, w, h);
        return false;
    }
    std::memcpy(staging_mapped_, data, expected);
    return submit_upload(ctx);
}

bool FrameUploader::submit_upload(VulkanContext& ctx) {
    VK_CHECK(vkResetCommandBuffer(cmd_buf_, 0));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd_buf_, &bi));

    barrier(cmd_buf_, image_,
            current_layout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyBufferToImage(cmd_buf_, staging_, image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    barrier(cmd_buf_, image_,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    current_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VK_CHECK(vkEndCommandBuffer(cmd_buf_));

    VK_CHECK(vkResetFences(ctx.device(), 1, &upload_fence_));
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_buf_;
    VK_CHECK(vkQueueSubmit(ctx.queue(), 1, &si, upload_fence_));
    VK_CHECK(vkWaitForFences(ctx.device(), 1, &upload_fence_, VK_TRUE, UINT64_MAX));
    return true;
}

}  // namespace otgcam
