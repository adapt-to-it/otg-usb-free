#pragma once

#include <vulkan/vulkan.h>
#include <android/native_window.h>

#include <cstdint>
#include <vector>

namespace otgcam {

class VulkanContext;

// VulkanSwapchain incapsula VkSurfaceKHR + VkSwapchainKHR + image views.
// Una sola istanza per processo. Distruggere prima di chiudere il
// VulkanContext.
class VulkanSwapchain {
public:
    VulkanSwapchain() = default;
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    bool create(VulkanContext& ctx, ANativeWindow* window);
    bool recreate(VulkanContext& ctx, ANativeWindow* window);
    void destroy(VulkanContext& ctx);

    bool is_ready() const { return swapchain_ != VK_NULL_HANDLE; }

    VkSurfaceKHR    surface()       const { return surface_; }
    VkSwapchainKHR  swapchain()     const { return swapchain_; }
    VkFormat        color_format()  const { return color_format_; }
    VkColorSpaceKHR color_space()   const { return color_space_; }
    VkExtent2D      extent()        const { return extent_; }
    uint32_t        image_count()   const { return static_cast<uint32_t>(images_.size()); }

    const std::vector<VkImage>&     images()      const { return images_; }
    const std::vector<VkImageView>& image_views() const { return image_views_; }

private:
    bool create_surface(VulkanContext& ctx, ANativeWindow* window);
    bool create_swapchain(VulkanContext& ctx);
    bool create_image_views(VulkanContext& ctx);
    void destroy_image_views(VulkanContext& ctx);
    void destroy_swapchain_only(VulkanContext& ctx);

    VkSurfaceKHR                  surface_       = VK_NULL_HANDLE;
    VkSwapchainKHR                swapchain_     = VK_NULL_HANDLE;
    VkFormat                      color_format_  = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR               color_space_   = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D                    extent_        = {0, 0};
    VkSurfaceTransformFlagBitsKHR pre_transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkPresentModeKHR              present_mode_  = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>          images_;
    std::vector<VkImageView>      image_views_;
};

}  // namespace otgcam
