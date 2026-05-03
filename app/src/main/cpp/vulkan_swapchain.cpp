#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "vulkan_swapchain.h"

#include <algorithm>
#include <cstring>

#include "utils/log.h"
#include "utils/vk_check.h"
#include "vulkan_context.h"

namespace otgcam {

namespace {

// Preferiamo R8G8B8A8_UNORM con sRGB nonlinear: e' sempre disponibile sulle
// implementazioni Android e ci permette di scrivere direttamente da compute
// shader senza conversioni implicite di gamma.
VkSurfaceFormatKHR pick_surface_format(const std::vector<VkSurfaceFormatKHR>& fmts) {
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM,
                                             VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                        : fmts.front();
}

}  // namespace

VulkanSwapchain::~VulkanSwapchain() {
    // destroy() richiede il VulkanContext quindi non possiamo farlo qui.
    // Il chiamante deve invocare destroy() esplicitamente. In debug logghiamo
    // un warning se si arriva qui con risorse vive.
    if (swapchain_ != VK_NULL_HANDLE || surface_ != VK_NULL_HANDLE) {
        OTGCAM_LOGW("VulkanSwapchain destructor called with live handles "
                    "(missing destroy() call?)");
    }
}

bool VulkanSwapchain::create(VulkanContext& ctx, ANativeWindow* window) {
    if (window == nullptr) {
        OTGCAM_LOGE("VulkanSwapchain::create: ANativeWindow is null");
        return false;
    }
    if (!create_surface(ctx, window)) return false;
    if (!create_swapchain(ctx)) return false;
    if (!create_image_views(ctx)) return false;
    OTGCAM_LOGI("Swapchain ready: %ux%u, %u images, format=%d",
                extent_.width, extent_.height,
                static_cast<unsigned>(images_.size()),
                static_cast<int>(color_format_));
    return true;
}

bool VulkanSwapchain::recreate(VulkanContext& ctx, ANativeWindow* window) {
    vkDeviceWaitIdle(ctx.device());
    destroy_image_views(ctx);
    destroy_swapchain_only(ctx);
    // surface_ resta valida se window e' la stessa. Se window cambia,
    // ricreiamo anche la surface per sicurezza.
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(ctx.instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    return create(ctx, window);
}

void VulkanSwapchain::destroy(VulkanContext& ctx) {
    if (ctx.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx.device());
    }
    destroy_image_views(ctx);
    destroy_swapchain_only(ctx);
    if (surface_ != VK_NULL_HANDLE && ctx.instance() != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(ctx.instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    extent_ = {0, 0};
    color_format_ = VK_FORMAT_UNDEFINED;
}

bool VulkanSwapchain::create_surface(VulkanContext& ctx, ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = window;
    VK_CHECK(vkCreateAndroidSurfaceKHR(ctx.instance(), &ci, nullptr, &surface_));

    VkBool32 supported = VK_FALSE;
    VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physical_device(),
                                                  ctx.queue_family_index(),
                                                  surface_, &supported));
    if (!supported) {
        OTGCAM_LOGE("Queue family %u does not support presenting on this surface",
                    ctx.queue_family_index());
        return false;
    }
    return true;
}

bool VulkanSwapchain::create_swapchain(VulkanContext& ctx) {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physical_device(),
                                                       surface_, &caps));

    uint32_t fmt_count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical_device(),
                                                  surface_, &fmt_count, nullptr));
    std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical_device(),
                                                  surface_, &fmt_count, fmts.data()));
    auto chosen_fmt = pick_surface_format(fmts);
    color_format_ = chosen_fmt.format;
    color_space_  = chosen_fmt.colorSpace;

    // FIFO e' sempre presente e da' v-sync; per il preview USB e' la scelta
    // giusta (no tearing, latenza accettabile a 30/60fps).
    present_mode_ = VK_PRESENT_MODE_FIFO_KHR;

    // Pre-rotation: usiamo currentTransform come richiesto da Android per
    // evitare rotazioni implicite del compositor. La rotazione utente
    // (Settings.rotation) e' indipendente e applicata nel compute shader.
    pre_transform_ = (caps.supportedTransforms & caps.currentTransform)
        ? caps.currentTransform
        : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX || extent_.height == UINT32_MAX) {
        // Implementation-defined: dovremmo prendere dimensioni dalla view.
        // Fallback su minImageExtent.
        extent_ = caps.minImageExtent;
    }
    extent_.width  = std::clamp(extent_.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    extent_.height = std::clamp(extent_.height, caps.minImageExtent.height, caps.maxImageExtent.height);

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    // Usage: TRANSFER_DST per il path step 3 (clear color via vkCmdClearColorImage),
    // STORAGE per il path step 4+ (compute shader scrive direttamente).
    const VkImageUsageFlags wanted_usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if ((caps.supportedUsageFlags & wanted_usage) != wanted_usage) {
        OTGCAM_LOGE("Surface usage flags missing: caps=0x%x wanted=0x%x",
                    caps.supportedUsageFlags, wanted_usage);
        return false;
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = color_format_;
    sci.imageColorSpace = color_space_;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = wanted_usage;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = pre_transform_;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = present_mode_;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(ctx.device(), &sci, nullptr, &swapchain_));

    uint32_t actual = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(ctx.device(), swapchain_, &actual, nullptr));
    images_.resize(actual);
    VK_CHECK(vkGetSwapchainImagesKHR(ctx.device(), swapchain_, &actual, images_.data()));
    return true;
}

bool VulkanSwapchain::create_image_views(VulkanContext& ctx) {
    image_views_.resize(images_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = images_[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = color_format_;
        ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                         VK_COMPONENT_SWIZZLE_IDENTITY,
                         VK_COMPONENT_SWIZZLE_IDENTITY,
                         VK_COMPONENT_SWIZZLE_IDENTITY};
        ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel = 0;
        ci.subresourceRange.levelCount = 1;
        ci.subresourceRange.baseArrayLayer = 0;
        ci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &ci, nullptr, &image_views_[i]));
    }
    return true;
}

void VulkanSwapchain::destroy_image_views(VulkanContext& ctx) {
    if (ctx.device() == VK_NULL_HANDLE) {
        image_views_.clear();
        return;
    }
    for (auto v : image_views_) {
        if (v != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx.device(), v, nullptr);
        }
    }
    image_views_.clear();
}

void VulkanSwapchain::destroy_swapchain_only(VulkanContext& ctx) {
    if (swapchain_ != VK_NULL_HANDLE && ctx.device() != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx.device(), swapchain_, nullptr);
    }
    swapchain_ = VK_NULL_HANDLE;
    images_.clear();
}

}  // namespace otgcam
