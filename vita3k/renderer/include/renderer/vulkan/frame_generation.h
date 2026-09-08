#pragma once

#ifdef __ANDROID__

#include <vulkan/vulkan_core.h>

#include <memory>
#include <string>
#include <vector>

namespace renderer::vulkan {

struct VKState;

bool configure_frame_generation_runtime(bool enabled, int multiplier, const std::string& cache_dir);
bool frame_generation_active();
int frame_generation_active_multiplier();
std::string frame_generation_last_error();

class FrameGenerationPresenter {
public:
    explicit FrameGenerationPresenter(VKState& state);
    ~FrameGenerationPresenter();

    FrameGenerationPresenter(const FrameGenerationPresenter&) = delete;
    FrameGenerationPresenter& operator=(const FrameGenerationPresenter&) = delete;

    bool configure_for_swapchain(
        VkSwapchainKHR swapchain,
        VkExtent2D extent,
        VkFormat swapchain_format,
        const std::vector<VkImage>& swapchain_images,
        bool swapchain_transfer_compatible);

    void release_swapchain();
    void shutdown();

    bool active() const;
    int multiplier() const;

    VkResult present(VkSemaphore render_ready, uint32_t real_image_index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace renderer::vulkan

#endif
