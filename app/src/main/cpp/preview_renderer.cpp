#include "preview_renderer.h"

#include <utility>

#include "compute_pipeline.h"
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

    // Calcolo dst e src crop in base all'aspect:
    //   fit     -> dst = sotto-rect centrato della swapchain, src = full.
    //              (aggiunge bande se aspect ratio differisce)
    //   fill    -> dst = swapchain full, src = sotto-rect centrato della cam.
    //              (CROP: parte dell'immagine fuori schermo)
    //   stretch -> dst = full, src = full (deformazione).
    int32_t dw = sw, dh = sh;
    int32_t src_crop_w = src_w, src_crop_h = src_h;
    if (xform.aspect == 0 /*fit*/) {
        const double sa = static_cast<double>(eff_w) / eff_h;
        const double da = static_cast<double>(sw) / sh;
        if (sa > da) { dw = sw; dh = static_cast<int32_t>(sw / sa); }
        else         { dh = sh; dw = static_cast<int32_t>(sh * sa); }
    } else if (xform.aspect == 1 /*fill*/) {
        // dst sempre full, croppiamo la src.
        dw = sw; dh = sh;
        // aspect dst (post-rotation eff): vogliamo che il src crop abbia
        // ratio = ratio_dst/effW_to_effH. Calcoliamo il crop in spazio
        // pre-rotation: se rotation 90/270 lo applichiamo invertito.
        const double da = static_cast<double>(sw) / sh;       // dst aspect
        const double sa_eff = static_cast<double>(eff_w) / eff_h; // src aspect (post-rot)
        if (sa_eff > da) {
            // src troppo largo (post-rot): croppiamo i lati.
            int32_t crop_eff_w = static_cast<int32_t>(eff_h * da);
            // riportiamo a coord src (pre-rotation): se swap, eff_w==src_h.
            if (swap) { src_crop_h = crop_eff_w; src_crop_w = src_w; }
            else      { src_crop_w = crop_eff_w; src_crop_h = src_h; }
        } else {
            // src troppo alto: croppiamo top/bottom.
            int32_t crop_eff_h = static_cast<int32_t>(eff_w / da);
            if (swap) { src_crop_w = crop_eff_h; src_crop_h = src_h; }
            else      { src_crop_h = crop_eff_h; src_crop_w = src_w; }
        }
    }
    // stretch: tutto pieno (default dei valori sopra).
    int32_t ox = (sw - dw) / 2;
    int32_t oy = (sh - dh) / 2;

    // Src offsets: parto dal crop centrato, poi applico mirror.
    int32_t scrop_ox = (src_w - src_crop_w) / 2;
    int32_t scrop_oy = (src_h - src_crop_h) / 2;
    int32_t sx0 = scrop_ox, sy0 = scrop_oy;
    int32_t sx1 = scrop_ox + src_crop_w, sy1 = scrop_oy + src_crop_h;
    if (xform.mirror_x) std::swap(sx0, sx1);
    if (xform.mirror_y) std::swap(sy0, sy1);
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
            blit.srcOffsets[0] = {sx1, sy1, 0};
            blit.srcOffsets[1] = {sx0, sy0, 1};
            blit.dstOffsets[0] = {dx0, dy0, 0};
            blit.dstOffsets[1] = {dx1, dy1, 1};
            break;
        case 90:
            blit.srcOffsets[0] = {sx1, sy0, 0};
            blit.srcOffsets[1] = {sx0, sy1, 1};
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

bool PreviewRenderer::render_frame_compute(VulkanContext& ctx,
                                           FrameUploader& uploader,
                                           PostProcessPipeline& pp,
                                           const FrameTransform& xform,
                                           const EnhancementParams& enh,
                                           const ClearColor& bg) {
    if (!swapchain_.is_ready() || !uploader.yuyv_ready() || !pp.is_ready()) {
        return false;
    }

    VK_CHECK(vkWaitForFences(ctx.device(), 1, &in_flight_fence_, VK_TRUE, UINT64_MAX));

    uint32_t image_index = 0;
    VkResult acquire_r = vkAcquireNextImageKHR(ctx.device(), swapchain_.swapchain(),
                                               UINT64_MAX, img_acquired_sem_,
                                               VK_NULL_HANDLE, &image_index);
    if (acquire_r == VK_ERROR_OUT_OF_DATE_KHR) {
        OTGCAM_LOGI("Swapchain out-of-date during acquire (compute); caller should recreate");
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

    // Step 1: dispatch compute (YUYV->RGBA + downscale + denoise/sharpen/defog).
    PostProcessPipeline::DispatchParams dp{};
    dp.src_w = uploader.width();
    dp.src_h = uploader.height();
    dp.sharpen = enh.sharpen;
    dp.denoise = enh.denoise;
    dp.defog   = enh.defog;
    dp.defog_enabled = enh.defog_enabled;
    pp.record(cmd_buffer_, dp);

    // Step 2: blit dalla dst del compute alla swapchain.
    VkImage target = swapchain_.images()[image_index];
    const auto sw_extent = swapchain_.extent();

    image_barrier(cmd_buffer_, target,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

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

    const int32_t src_w = static_cast<int32_t>(pp.dst_width());
    const int32_t src_h = static_cast<int32_t>(pp.dst_height());
    const int32_t sw    = static_cast<int32_t>(sw_extent.width);
    const int32_t sh    = static_cast<int32_t>(sw_extent.height);

    const int  rot   = ((xform.rotation % 360) + 360) % 360;
    const bool swap  = (rot == 90 || rot == 270);
    const int32_t eff_w = swap ? src_h : src_w;
    const int32_t eff_h = swap ? src_w : src_h;

    int32_t dw = sw, dh = sh;
    int32_t src_crop_w = src_w, src_crop_h = src_h;
    if (xform.aspect == 0 /*fit*/) {
        const double sa = static_cast<double>(eff_w) / eff_h;
        const double da = static_cast<double>(sw) / sh;
        if (sa > da) { dw = sw; dh = static_cast<int32_t>(sw / sa); }
        else         { dh = sh; dw = static_cast<int32_t>(sh * sa); }
    } else if (xform.aspect == 1 /*fill*/) {
        dw = sw; dh = sh;
        const double da = static_cast<double>(sw) / sh;
        const double sa_eff = static_cast<double>(eff_w) / eff_h;
        if (sa_eff > da) {
            int32_t crop_eff_w = static_cast<int32_t>(eff_h * da);
            if (swap) { src_crop_h = crop_eff_w; src_crop_w = src_w; }
            else      { src_crop_w = crop_eff_w; src_crop_h = src_h; }
        } else {
            int32_t crop_eff_h = static_cast<int32_t>(eff_w / da);
            if (swap) { src_crop_w = crop_eff_h; src_crop_h = src_h; }
            else      { src_crop_h = crop_eff_h; src_crop_w = src_w; }
        }
    }
    int32_t ox = (sw - dw) / 2;
    int32_t oy = (sh - dh) / 2;

    int32_t scrop_ox = (src_w - src_crop_w) / 2;
    int32_t scrop_oy = (src_h - src_crop_h) / 2;
    int32_t sx0 = scrop_ox, sy0 = scrop_oy;
    int32_t sx1 = scrop_ox + src_crop_w, sy1 = scrop_oy + src_crop_h;
    if (xform.mirror_x) std::swap(sx0, sx1);
    if (xform.mirror_y) std::swap(sy0, sy1);
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
            blit.srcOffsets[0] = {sx1, sy1, 0};
            blit.srcOffsets[1] = {sx0, sy0, 1};
            blit.dstOffsets[0] = {dx0, dy0, 0};
            blit.dstOffsets[1] = {dx1, dy1, 1};
            break;
        case 90:
            blit.srcOffsets[0] = {sx1, sy0, 0};
            blit.srcOffsets[1] = {sx0, sy1, 1};
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
                   pp.dst_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   target,         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

    image_barrier(cmd_buffer_, target,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    VK_CHECK(vkEndCommandBuffer(cmd_buffer_));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                      VK_PIPELINE_STAGE_TRANSFER_BIT;
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
        OTGCAM_LOGE("vkQueuePresentKHR (compute) failed: %s", vk_result_string(present_r));
        return false;
    }
    return true;
}

}  // namespace otgcam
