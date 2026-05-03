#include "preview_renderer.h"

#include <utility>

#include "frame_uploader.h"
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

bool PreviewRenderer::render_frame(VulkanContext& ctx,
                                   FrameUploader& uploader,
                                   const FrameTransform& xform,
                                   const ClearColor& bg) {
    if (!swapchain_.is_ready() || !uploader.is_ready()) {
        return false;
    }

    VK_CHECK(vkWaitForFences(ctx.device(), 1, &in_flight_fence_, VK_TRUE, UINT64_MAX));

    uint32_t image_index = 0;
    VkResult acquire_r = vkAcquireNextImageKHR(ctx.device(), swapchain_.swapchain(),
                                               UINT64_MAX, img_acquired_sem_,
                                               VK_NULL_HANDLE, &image_index);
    if (acquire_r == VK_ERROR_OUT_OF_DATE_KHR) {
        OTGCAM_LOGI("Swapchain out-of-date during acquire (frame); caller should recreate");
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
    const auto sw_extent = swapchain_.extent();

    image_barrier(cmd_buffer_, target,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Background fill (letterbox) prima del blit.
    VkClearColorValue clear{};
    clear.float32[0] = bg.r;
    clear.float32[1] = bg.g;
    clear.float32[2] = bg.b;
    clear.float32[3] = bg.a;
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cmd_buffer_, target,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    // Source dims (con eventuale swap per rotation 90/270 al fine del fit).
    const int32_t src_w = static_cast<int32_t>(uploader.width());
    const int32_t src_h = static_cast<int32_t>(uploader.height());
    const int32_t sw    = static_cast<int32_t>(sw_extent.width);
    const int32_t sh    = static_cast<int32_t>(sw_extent.height);

    // Quando l'immagine ruota di 90/270 deg, l'aspect ratio "effettivo"
    // ai fini del fit/fill nella swapchain e' invertito.
    const int  rot   = ((xform.rotation % 360) + 360) % 360;
    const bool swap  = (rot == 90 || rot == 270);
    const int32_t eff_w = swap ? src_h : src_w;
    const int32_t eff_h = swap ? src_w : src_h;

    // Calcolo del rettangolo di destinazione in base ad aspect.
    int32_t dw = sw, dh = sh;
    if (xform.aspect == 0 /*fit*/ || xform.aspect == 1 /*fill*/) {
        const double sa = static_cast<double>(eff_w) / eff_h;
        const double da = static_cast<double>(sw) / sh;
        const bool wider_src = sa > da;
        // fit: il src e' contenuto -> width o height match
        // fill: il src riempie    -> opposto
        const bool match_width = (xform.aspect == 0) ? wider_src : !wider_src;
        if (match_width) {
            dw = sw;
            dh = static_cast<int32_t>(sw / sa);
        } else {
            dh = sh;
            dw = static_cast<int32_t>(sh * sa);
        }
    }
    // stretch: dw=sw, dh=sh (default).
    int32_t ox = (sw - dw) / 2;
    int32_t oy = (sh - dh) / 2;

    // Sorgente: per rotation usiamo la rimappatura degli srcOffsets.
    // vkCmdBlitImage interpreta srcOffsets[0]/srcOffsets[1] come due
    // angoli opposti del rettangolo sorgente; per ruotare l'immagine
    // di 90/180/270 deg orientiamo gli offset di conseguenza, e per
    // il mirror invertiamo le coordinate dell'asse selezionato.
    int32_t sx0 = 0, sy0 = 0, sx1 = src_w, sy1 = src_h;
    if (xform.mirror_x) std::swap(sx0, sx1);
    if (xform.mirror_y) std::swap(sy0, sy1);

    // Per la rotation usiamo il dst con dimensioni gia' adatte (eff_w/eff_h
    // sopra) ed effettuiamo la rotazione dei dst-offsets attorno al centro.
    // vkCmdBlitImage permette di "ruotare di 180" invertendo entrambi gli
    // assi; per 90/270 si scambia (x,y) tra src e dst tramite un trucco:
    // disponiamo gli offset di destinazione in modo che la mappatura
    // src->dst rifletta la rotazione voluta. Per semplicita' di
    // implementazione gestiamo le 4 rotazioni canoniche con un branch.
    int32_t dx0 = ox, dy0 = oy, dx1 = ox + dw, dy1 = oy + dh;

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;

    switch (rot) {
        case 0:
            blit.srcOffsets[0] = {sx0, sy0, 0};
            blit.srcOffsets[1] = {sx1, sy1, 1};
            blit.dstOffsets[0] = {dx0, dy0, 0};
            blit.dstOffsets[1] = {dx1, dy1, 1};
            break;
        case 180:
            // Equivalente a flip x+y: invertiamo entrambi i src.
            blit.srcOffsets[0] = {sx1, sy1, 0};
            blit.srcOffsets[1] = {sx0, sy0, 1};
            blit.dstOffsets[0] = {dx0, dy0, 0};
            blit.dstOffsets[1] = {dx1, dy1, 1};
            break;
        case 90:
            // Per ruotare 90 CCW, mappiamo:
            //   dst.x  <- src.y
            //   dst.y  <- (src_w - src.x)
            // vkCmdBlitImage non ruota direttamente, quindi usiamo offsets
            // in modo che l'asse src "x" scorra lungo l'asse dst "y" e
            // viceversa. Questo e' realizzabile scegliendo srcOffsets
            // sull'asse y per il primo punto e sull'asse x per il secondo,
            // accoppiati con i dstOffsets corrispondenti dell'altro asse.
            blit.srcOffsets[0] = {sx1, sy0, 0};
            blit.srcOffsets[1] = {sx0, sy1, 1};
            // Scambio x/y nel dst: il blit interpreta correttamente.
            blit.dstOffsets[0] = {dx1, dy0, 0};
            blit.dstOffsets[1] = {dx0, dy1, 1};
            break;
        case 270:
        default:
            blit.srcOffsets[0] = {sx0, sy1, 0};
            blit.srcOffsets[1] = {sx1, sy0, 1};
            blit.dstOffsets[0] = {dx1, dy0, 0};
            blit.dstOffsets[1] = {dx0, dy1, 1};
            break;
    }

    vkCmdBlitImage(cmd_buffer_,
                   uploader.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   target,            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

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
        return false;
    }
    if (present_r != VK_SUCCESS) {
        OTGCAM_LOGE("vkQueuePresentKHR (frame) failed: %s", vk_result_string(present_r));
        return false;
    }
    return true;
}

}  // namespace otgcam
