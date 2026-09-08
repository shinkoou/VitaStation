#ifdef __ANDROID__

#include <renderer/vulkan/frame_generation.h>

#include <renderer/vulkan/state.h>
#include <util/log.h>

#include <lsfg_3_1.hpp>

#include <android/hardware_buffer.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vitafs = std::filesystem;

namespace renderer::vulkan {
namespace {

struct RuntimeSettings {
    bool enabled = false;
    int multiplier = 2;
    std::string cache_dir;
    uint64_t generation = 0;
};

std::mutex g_runtime_mutex;
RuntimeSettings g_runtime;
std::atomic<bool> g_active{ false };
std::atomic<int> g_active_multiplier{ 1 };
std::mutex g_error_mutex;
std::string g_last_error;

RuntimeSettings runtime_snapshot() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_runtime;
}

void set_last_error(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        g_last_error = message;
    }
    if (!message.empty())
        LOG_WARN("VitaStation LSFG: {}", message);
}

void set_active(const bool active, const int multiplier = 1) {
    g_active.store(active, std::memory_order_release);
    g_active_multiplier.store(active ? std::max(multiplier, 2) : 1, std::memory_order_release);
}

uint64_t device_uuid(const VKState& state) {
    return (static_cast<uint64_t>(state.physical_device_properties.vendorID) << 32u)
        | static_cast<uint64_t>(state.physical_device_properties.deviceID);
}

const std::unordered_map<std::string, uint32_t>& shader_name_table() {
    static const std::unordered_map<std::string, uint32_t> table = {
        { "mipmaps", 255 },
        { "generate", 256 },
        { "gamma[0]", 257 },
        { "delta[0]", 257 },
        { "delta[5]", 258 },
        { "gamma[1]", 259 },
        { "gamma[2]", 260 },
        { "gamma[3]", 261 },
        { "gamma[4]", 262 },
        { "delta[1]", 263 },
        { "delta[2]", 264 },
        { "delta[3]", 265 },
        { "delta[4]", 266 },
        { "alpha[0]", 267 },
        { "alpha[1]", 268 },
        { "alpha[2]", 269 },
        { "alpha[3]", 270 },
        { "delta[6]", 271 },
        { "delta[7]", 272 },
        { "delta[8]", 273 },
        { "delta[9]", 274 },
        { "beta[0]", 275 },
        { "beta[1]", 276 },
        { "beta[2]", 277 },
        { "beta[3]", 278 },
        { "beta[4]", 279 },
    };
    return table;
}

std::vector<uint8_t> read_shader(const std::string& cache_dir, const std::string& name) {
    const auto it = shader_name_table().find(name);
    if (it == shader_name_table().end())
        throw std::runtime_error("unknown LSFG shader name: " + name);

    const vitafs::path path = vitafs::path(cache_dir) / (std::to_string(it->second) + ".spv");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("missing LSFG shader cache: " + path.string());

    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());

    if (bytes.size() < sizeof(uint32_t))
        throw std::runtime_error("invalid LSFG shader cache: " + path.string());

    uint32_t magic = 0;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    if (magic != 0x07230203u)
        throw std::runtime_error("LSFG shader cache is not SPIR-V: " + path.string());

    return bytes;
}

void check_vk(const VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(
            std::string(operation) + " failed with VkResult " + std::to_string(static_cast<int>(result)));
}

struct AhbImage {
    VKState* state = nullptr;
    AHardwareBuffer* ahb = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageLayout resting_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    AhbImage() = default;
    AhbImage(const AhbImage&) = delete;
    AhbImage& operator=(const AhbImage&) = delete;

    ~AhbImage() {
        destroy();
    }

    void destroy() {
        if (!state)
            return;

        const VkDevice device = static_cast<VkDevice>(state->device);
        if (image != VK_NULL_HANDLE) {
            VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
        if (ahb) {
            AHardwareBuffer_release(ahb);
            ahb = nullptr;
        }

        resting_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        state = nullptr;
    }

    void create(VKState& vk_state, const VkExtent2D extent, const VkFormat format) {
        destroy();
        state = &vk_state;

        uint32_t ahb_format = 0;
        switch (format) {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            ahb_format = AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
            ahb_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            break;
        default:
            throw std::runtime_error("unsupported AHardwareBuffer format for LSFG");
        }

        const AHardwareBuffer_Desc ahb_desc{
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
            .format = ahb_format,
            .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
                | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
            .stride = 0,
            .rfu0 = 0,
            .rfu1 = 0,
        };

        if (AHardwareBuffer_allocate(&ahb_desc, &ahb) != 0 || !ahb)
            throw std::runtime_error("AHardwareBuffer allocation failed for LSFG");

        const VkExternalMemoryImageCreateInfo external_info{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
        };

        const VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &external_info,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { extent.width, extent.height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT
                | VK_IMAGE_USAGE_STORAGE_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        const VkDevice device = static_cast<VkDevice>(vk_state.device);
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImage(device, &image_info, nullptr, &image),
            "vkCreateImage(AHB)");

        VkMemoryRequirements requirements{};
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetImageMemoryRequirements(device, image, &requirements);

        uint32_t memory_type = UINT32_MAX;
        for (uint32_t index = 0; index < vk_state.physical_device_memory.memoryTypeCount; ++index) {
            const auto flags = vk_state.physical_device_memory.memoryTypes[index].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << index))
                && static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                memory_type = index;
                break;
            }
        }

        if (memory_type == UINT32_MAX) {
            for (uint32_t index = 0; index < vk_state.physical_device_memory.memoryTypeCount; ++index) {
                if (requirements.memoryTypeBits & (1u << index)) {
                    memory_type = index;
                    break;
                }
            }
        }

        if (memory_type == UINT32_MAX)
            throw std::runtime_error("no compatible Vulkan memory type for LSFG AHB");

        const VkMemoryDedicatedAllocateInfo dedicated_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = nullptr,
            .image = image,
            .buffer = VK_NULL_HANDLE,
        };
        const VkImportAndroidHardwareBufferInfoANDROID import_info{
            .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .pNext = &dedicated_info,
            .buffer = ahb,
        };
        const VkMemoryAllocateInfo allocation_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import_info,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memory_type,
        };

        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory(device, &allocation_info, nullptr, &memory),
            "vkAllocateMemory(AHB)");
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkBindImageMemory(device, image, memory, 0),
            "vkBindImageMemory(AHB)");
    }
};

} // namespace

bool configure_frame_generation_runtime(
    const bool enabled,
    const int multiplier,
    const std::string& cache_dir) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    g_runtime.enabled = enabled;
    g_runtime.multiplier = enabled ? 2 : std::max(multiplier, 2);
    g_runtime.cache_dir = cache_dir;
    ++g_runtime.generation;

    if (!enabled)
        set_active(false);

    return true;
}

bool frame_generation_active() {
    return g_active.load(std::memory_order_acquire);
}

int frame_generation_active_multiplier() {
    return g_active_multiplier.load(std::memory_order_acquire);
}

std::string frame_generation_last_error() {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    return g_last_error;
}

struct FrameGenerationPresenter::Impl {
    explicit Impl(VKState& state)
        : state(state) {
    }

    VKState& state;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> swapchain_images;

    AhbImage input_0;
    AhbImage input_1;
    AhbImage output_0;

    VkCommandBuffer copy_command_buffer = VK_NULL_HANDLE;
    VkSemaphore generated_acquire = VK_NULL_HANDLE;

    int32_t context_id = -1;
    bool library_initialized = false;
    bool session_active = false;
    bool session_failed = false;
    int session_multiplier = 2;
    uint64_t frame_index = 0;
    std::string library_cache_dir;

    VkDevice device() const {
        return static_cast<VkDevice>(state.device);
    }

    VkPhysicalDevice physical_device() const {
        return static_cast<VkPhysicalDevice>(state.physical_device);
    }

    VkQueue queue() const {
        return static_cast<VkQueue>(state.general_queue);
    }

    void create_sync_objects() {
        const VkCommandBufferAllocateInfo command_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = static_cast<VkCommandPool>(state.general_command_pool),
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateCommandBuffers(
                device(), &command_info, &copy_command_buffer),
            "vkAllocateCommandBuffers(LSFG)");

        const VkSemaphoreCreateInfo semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(
                device(), &semaphore_info, nullptr, &generated_acquire),
            "vkCreateSemaphore(LSFG acquire)");
    }

    void destroy_sync_objects() {
        if (generated_acquire != VK_NULL_HANDLE) {
            VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(device(), generated_acquire, nullptr);
            generated_acquire = VK_NULL_HANDLE;
        }
        if (copy_command_buffer != VK_NULL_HANDLE) {
            VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeCommandBuffers(
                device(),
                static_cast<VkCommandPool>(state.general_command_pool),
                1,
                &copy_command_buffer);
            copy_command_buffer = VK_NULL_HANDLE;
        }
    }

    void initialize_library(const std::string& cache_dir) {
        if (library_initialized && library_cache_dir == cache_dir)
            return;

        if (library_initialized) {
            LSFG_3_1::waitIdle();
            LSFG_3_1::finalize();
            library_initialized = false;
            library_cache_dir.clear();
        }

        LSFG_3_1::initialize(
            device_uuid(state),
            false,
            1.0f,
            1,
            [cache_dir](const std::string& shader_name) {
                return read_shader(cache_dir, shader_name);
            });

        library_initialized = true;
        library_cache_dir = cache_dir;
    }

    void begin_copy_commands() {
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkResetCommandBuffer(copy_command_buffer, 0),
            "vkResetCommandBuffer(LSFG)");

        const VkCommandBufferBeginInfo begin_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        };
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkBeginCommandBuffer(copy_command_buffer, &begin_info),
            "vkBeginCommandBuffer(LSFG)");
    }

    void end_copy_commands() {
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkEndCommandBuffer(copy_command_buffer),
            "vkEndCommandBuffer(LSFG)");
    }

    void initialize_ahb_layout(AhbImage& image) {
        begin_copy_commands();

        const VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(
            copy_command_buffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);

        end_copy_commands();

        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &copy_command_buffer,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };

        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(queue(), 1, &submit, VK_NULL_HANDLE),
            "vkQueueSubmit(LSFG layout init)");
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueWaitIdle(queue()),
            "vkQueueWaitIdle(LSFG layout init)");

        image.resting_layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    void record_real_to_input(const VkImage real_image, AhbImage& input) {
        begin_copy_commands();

        const VkImageMemoryBarrier barriers[] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = real_image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = input.resting_layout,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = input.image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            },
        };

        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(
            copy_command_buffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            2,
            barriers);

        const VkImageBlit blit{
            .srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .srcOffsets = {
                { 0, 0, 0 },
                { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 },
            },
            .dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .dstOffsets = {
                { 0, 0, 0 },
                { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 },
            },
        };

        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBlitImage(
            copy_command_buffer,
            real_image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            input.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blit,
            VK_FILTER_NEAREST);

        const VkImageMemoryBarrier restore[] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = real_image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = input.resting_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = input.image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            },
        };

        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(
            copy_command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            2,
            restore);

        end_copy_commands();
    }

    void record_output_to_swapchain(const AhbImage& output, const VkImage generated_image) {
        begin_copy_commands();

        const VkImageMemoryBarrier barriers[] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = output.resting_layout,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = output.image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = generated_image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            },
        };

        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(
            copy_command_buffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            2,
            barriers);

        const VkImageBlit blit{
            .srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .srcOffsets = {
                { 0, 0, 0 },
                { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 },
            },
            .dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .dstOffsets = {
                { 0, 0, 0 },
                { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 },
            },
        };

        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBlitImage(
            copy_command_buffer,
            output.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            generated_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blit,
            VK_FILTER_NEAREST);

        const VkImageMemoryBarrier restore[] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = output.resting_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = output.image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = generated_image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            },
        };

        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(
            copy_command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            2,
            restore);

        end_copy_commands();
    }

    void submit_copy_waiting(const VkSemaphore wait) {
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1u : 0u,
            .pWaitSemaphores = wait != VK_NULL_HANDLE ? &wait : nullptr,
            .pWaitDstStageMask = wait != VK_NULL_HANDLE ? &wait_stage : nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &copy_command_buffer,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };

        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(queue(), 1, &submit, VK_NULL_HANDLE),
            "vkQueueSubmit(LSFG copy)");
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueWaitIdle(queue()),
            "vkQueueWaitIdle(LSFG copy)");
    }

    VkResult queue_present_no_wait(const uint32_t image_index) {
        const VkPresentInfoKHR present_info{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &image_index,
            .pResults = nullptr,
        };
        return VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(queue(), &present_info);
    }

    void mark_session_failed(const std::string& reason) {
        session_failed = true;
        session_active = false;
        set_active(false);
        set_last_error(reason);
    }

    void release_swapchain() {
        set_active(false);
        session_active = false;
        session_failed = false;
        frame_index = 0;

        if (swapchain == VK_NULL_HANDLE
            && context_id < 0
            && copy_command_buffer == VK_NULL_HANDLE) {
            return;
        }

        if (queue() != VK_NULL_HANDLE)
            VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueWaitIdle(queue());

        if (context_id >= 0) {
            try {
                LSFG_3_1::waitIdle();
                LSFG_3_1::deleteContext(context_id);
            } catch (const std::exception& e) {
                LOG_WARN("VitaStation LSFG context cleanup failed: {}", e.what());
            }
            context_id = -1;
        }

        destroy_sync_objects();
        output_0.destroy();
        input_1.destroy();
        input_0.destroy();

        swapchain_images.clear();
        swapchain = VK_NULL_HANDLE;
        swapchain_format = VK_FORMAT_UNDEFINED;
        extent = {};
    }

    void shutdown() {
        release_swapchain();
        if (library_initialized) {
            try {
                LSFG_3_1::waitIdle();
                LSFG_3_1::finalize();
            } catch (const std::exception& e) {
                LOG_WARN("VitaStation LSFG finalization failed: {}", e.what());
            }
            library_initialized = false;
            library_cache_dir.clear();
        }
    }

    bool configure_for_swapchain(
        const VkSwapchainKHR new_swapchain,
        const VkExtent2D new_extent,
        const VkFormat new_swapchain_format,
        const std::vector<VkImage>& new_images,
        const bool transfer_compatible) {
        const RuntimeSettings runtime = runtime_snapshot();

        release_swapchain();

        if (!runtime.enabled || runtime.cache_dir.empty())
            return false;

        if (!transfer_compatible) {
            set_last_error("swapchain does not support TRANSFER_SRC + TRANSFER_DST");
            return false;
        }

        if (!state.support_android_buffer_import) {
            set_last_error("VK_ANDROID_external_memory_android_hardware_buffer is unavailable");
            return false;
        }

        if (runtime.multiplier != 2) {
            set_last_error("Phase03B currently supports LSFG 2x only");
            return false;
        }

        const VkFormat internal_format = VK_FORMAT_R16G16B16A16_SFLOAT;

        VkFormatProperties internal_props{};
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties(
            physical_device(), internal_format, &internal_props);
        const VkFormatFeatureFlags internal_required =
            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((internal_props.optimalTilingFeatures & internal_required) != internal_required) {
            set_last_error("GPU does not expose FP16 blit support required by LSFG");
            return false;
        }

        VkFormatProperties swapchain_props{};
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties(
            physical_device(), new_swapchain_format, &swapchain_props);
        const VkFormatFeatureFlags swapchain_required =
            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((swapchain_props.optimalTilingFeatures & swapchain_required) != swapchain_required) {
            set_last_error("swapchain format does not support LSFG blits");
            return false;
        }

        try {
            swapchain = new_swapchain;
            extent = new_extent;
            swapchain_format = new_swapchain_format;
            swapchain_images = new_images;
            session_multiplier = 2;

            create_sync_objects();

            input_0.create(state, extent, internal_format);
            input_1.create(state, extent, internal_format);
            output_0.create(state, extent, internal_format);

            initialize_ahb_layout(input_0);
            initialize_ahb_layout(input_1);
            initialize_ahb_layout(output_0);

            initialize_library(runtime.cache_dir);

            std::vector<AHardwareBuffer*> outputs{ output_0.ahb };
            context_id = LSFG_3_1::createContextFromAHB(
                input_0.ahb,
                input_1.ahb,
                outputs,
                extent,
                internal_format);

            session_active = true;
            session_failed = false;
            frame_index = 0;
            set_last_error("");
            set_active(true, session_multiplier);

            LOG_INFO(
                "VitaStation LSFG 2x active: {}x{} on {}",
                extent.width,
                extent.height,
                state.physical_device_properties.deviceName.data());
            return true;
        } catch (const std::exception& e) {
            const std::string message = std::string("LSFG initialization failed: ") + e.what();
            LOG_ERROR("{}", message);
            release_swapchain();
            set_last_error(message);
            return false;
        }
    }

    VkResult present(const VkSemaphore render_ready, const uint32_t real_image_index) {
        if (!session_active || session_failed || context_id < 0
            || real_image_index >= swapchain_images.size()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        try {
            AhbImage& current_input = (frame_index % 2 == 0) ? input_0 : input_1;

            record_real_to_input(swapchain_images[real_image_index], current_input);
            submit_copy_waiting(render_ready);

            VkResult generated_result = VK_SUCCESS;

            // First frame primes input A. From the second real frame onward,
            // one LSFG frame is inserted between the previous and current real frame.
            if (frame_index > 0) {
                LSFG_3_1::presentContext(context_id, -1, {});
                LSFG_3_1::waitIdle();

                uint32_t generated_index = 0;
                const VkResult acquire_result =
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(
                        device(),
                        swapchain,
                        std::numeric_limits<uint64_t>::max(),
                        generated_acquire,
                        VK_NULL_HANDLE,
                        &generated_index);

                if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR)
                    return acquire_result;

                record_output_to_swapchain(output_0, swapchain_images.at(generated_index));
                submit_copy_waiting(generated_acquire);

                generated_result = queue_present_no_wait(generated_index);
                if (generated_result != VK_SUCCESS && generated_result != VK_SUBOPTIMAL_KHR)
                    return generated_result;
            }

            const VkResult real_result = queue_present_no_wait(real_image_index);
            ++frame_index;

            if (real_result != VK_SUCCESS && real_result != VK_SUBOPTIMAL_KHR)
                return real_result;

            return generated_result == VK_SUBOPTIMAL_KHR
                ? generated_result
                : real_result;
        } catch (const std::exception& e) {
            const std::string message = std::string("LSFG runtime failed: ") + e.what();
            mark_session_failed(message);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
};

FrameGenerationPresenter::FrameGenerationPresenter(VKState& state)
    : impl(std::make_unique<Impl>(state)) {
}

FrameGenerationPresenter::~FrameGenerationPresenter() {
    if (impl)
        impl->shutdown();
}

bool FrameGenerationPresenter::configure_for_swapchain(
    const VkSwapchainKHR swapchain,
    const VkExtent2D extent,
    const VkFormat swapchain_format,
    const std::vector<VkImage>& swapchain_images,
    const bool swapchain_transfer_compatible) {
    return impl->configure_for_swapchain(
        swapchain,
        extent,
        swapchain_format,
        swapchain_images,
        swapchain_transfer_compatible);
}

void FrameGenerationPresenter::release_swapchain() {
    impl->release_swapchain();
}

void FrameGenerationPresenter::shutdown() {
    impl->shutdown();
}

bool FrameGenerationPresenter::active() const {
    return impl->session_active && !impl->session_failed;
}

int FrameGenerationPresenter::multiplier() const {
    return active() ? impl->session_multiplier : 1;
}

VkResult FrameGenerationPresenter::present(
    const VkSemaphore render_ready,
    const uint32_t real_image_index) {
    return impl->present(render_ready, real_image_index);
}

} // namespace renderer::vulkan

#endif
