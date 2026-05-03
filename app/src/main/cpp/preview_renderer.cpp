#include "preview_renderer.h"

#include "utils/log.h"
#include "utils/vk_check.h"
#include "vulkan_context.h"

namespace otgcam {

namespace {

void image_barrier(VkCommandBuffer cb,
                   VkImage image,
                   VkImageLayout old_layout,
                   VkImageLayout new_layout,
                   VkAccessFlags src_access,
                   VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage,
                   VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0,
                         0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

PreviewRenderer::~PreviewRenderer() {
    if (cmd_pool_ != VK_NULL_HANDLE || swapchain_.is_ready()) {
        OTGCAM_LOGW("PreviewRenderer destructor: detach_surface() not called");
    }
}

bool PreviewRenderer::attach_surface(VulkanContext& ctx, ANativeWindow* window) {
    if (swapchain_.is_ready()) {
        // Idempotente: stessa surface --> ricreiamo (gestisce resize/rotation).
        if (!swapchain_.recreate(ctx, window)) {
            OTGCAM_LOGE("Swapchain recreate failed");
            return false;
        }
        return true;
    }
    if (!swapchain_.create(ctx, window)) {
        OTGCAM_LOGE("Swapchain create failed");
        return false;
    }
    if (!create_command_pool(ctx)) {
        OTGCAM_LOGE("Command pool create failed");
        detach_surface(ctx);
        return false;
    }
    if (!create_sync_objects(ctx)) {
        OTGCAM_LOGE("Sync objects create failed");
        detach_surface(ctx);
        return false;
    }
    OTGCAM_LOGI("PreviewRenderer attached: %ux%u",
                swapchain_.extent().width, swapchain_.extent().height);
    return true;
}

void PreviewRenderer::detach_surface(VulkanContext& ctx) {
    if (ctx.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx.device());
    }
    destroy_sync_objects(ctx);
    destroy_command_pool(ctx);
    swapchain_.destroy(ctx);
    OTGCAM_LOGI("PreviewRenderer detached");
}

bool PreviewRenderer::create_command_pool(VulkanContext& ctx) {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.queue_family_index();
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pci, nullptr, &cmd_pool_));

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &ai, &cmd_buffer_));
    return true;
}

bool PreviewRenderer::create_sync_objects(VulkanContext& ctx) {
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_CHECK(vkCreateSemaphore(ctx.device(), &si, nullptr, &img_acquired_sem_));
    VK_CHECK(vkCreateSemaphore(ctx.device(), &si, nullptr, &render_done_sem_));

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // primo frame: non bloccare wait
    VK_CHECK(vkCreateFence(ctx.device(), &fi, nullptr, &in_flight_fence_));
    return true;
}

void PreviewRenderer::destroy_sync_objects(VulkanContext& ctx) {
    if (ctx.device() == VK_NULL_HANDLE) {
        img_acquired_sem_ = VK_NULL_HANDLE;
        render_done_sem_  = VK_NULL_HANDLE;
        in_flight_fence_  = VK_NULL_HANDLE;
        return;
    }
    if (img_acquired_sem_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(ctx.device(), img_acquired_sem_, nullptr);
        img_acquired_sem_ = VK_NULL_HANDLE;
    }
    if (render_done_sem_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(ctx.device(), render_done_sem_, nullptr);
        render_done_sem_ = VK_NULL_HANDLE;
    }
    if (in_flight_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(ctx.device(), in_flight_fence_, nullptr);
        in_flight_fence_ = VK_NULL_HANDLE;
    }
}

void PreviewRenderer::destroy_command_pool(VulkanContext& ctx) {
    if (cmd_pool_ != VK_NULL_HANDLE && ctx.device() != VK_NULL_HANDLE) {
        vkDestroyCommandPool(ctx.device(), cmd_pool_, nullptr);
    }
    cmd_pool_ = VK_NULL_HANDLE;
    cmd_buffer_ = VK_NULL_HANDLE;
}

bool PreviewRenderer::render_clear(VulkanContext& ctx, const ClearColor& c) {
    if (!swapchain_.is_ready()) {
        OTGCAM_LOGW("render_clear called without an attached swapchain");
        return false;
    }

    VK_CHECK(vkWaitForFences(ctx.device(), 1, &in_flight_fence_, VK_TRUE, UINT64_MAX));

    uint32_t image_index = 0;
    VkResult acquire_r = vkAcquireNextImageKHR(ctx.device(), swapchain_.swapchain(),
                                               UINT64_MAX, img_acquired_sem_,
                                               VK_NULL_HANDLE, &image_index);
    if (acquire_r == VK_ERROR_OUT_OF_DATE_KHR) {
        OTGCAM_LOGI("Swapchain out-of-date during acquire; caller should recreate");
        return false;
    }
    if (acquire_r != VK_SUCCESS && acquire_r != VK_SUBOPTIMAL_KHR) {
        OTGCAM_LOGE("vkAcquireNextImageKHR failed: %s", vk_result_string(acquire_r));
        return false;
    }

    VK_CHECK(vkResetFences(ctx.device(), 1, &in_flight_fence_));
    VK_CHECK(vkResetCommandBuffer(cmd_buffer_, 0));

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd_buffer_, &bi));

    VkImage target = swapchain_.images()[image_index];

    image_barrier(cmd_buffer_, target,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkClearColorValue clear{};
    clear.float32[0] = c.r;
    clear.float32[1] = c.g;
    clear.float32[2] = c.b;
    clear.float32[3] = c.a;
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    vkCmdClearColorImage(cmd_buffer_, target,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    image_barrier(cmd_buffer_, target,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    VK_CHECK(vkEndCommandBuffer(cmd_buffer_));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &img_acquired_sem_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_buffer_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_done_sem_;
    VK_CHECK(vkQueueSubmit(ctx.queue(), 1, &si, in_flight_fence_));

    VkSwapchainKHR sc = swapchain_.swapchain();
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_done_sem_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &image_index;
    VkResult present_r = vkQueuePresentKHR(ctx.queue(), &pi);
    if (present_r == VK_ERROR_OUT_OF_DATE_KHR || present_r == VK_SUBOPTIMAL_KHR) {
        OTGCAM_LOGI("Swapchain suboptimal/out-of-date during present");
        return false;
    }
    if (present_r != VK_SUCCESS) {
        OTGCAM_LOGE("vkQueuePresentKHR failed: %s", vk_result_string(present_r));
        return false;
    }
    return true;
}

}  // namespace otgcam
