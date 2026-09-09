#ifdef __ANDROID__

#include <renderer/vulkan/frame_generation.h>

#include <renderer/vulkan/state.h>
#include <util/log.h>

#include <lsfg_3_1p.hpp>

#include <android/hardware_buffer.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cmath>
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

// VitaStation FrameGen 2.0 scheduler.
// This is intentionally independent from LSFG internals: the emulator owns the
// real-frame cadence and can decide when interpolation is safe before invoking
// the backend.
struct FrameTimingHistory {
    static constexpr size_t kCapacity = 120;
    static constexpr size_t kMinimumSamples = 12;
    static constexpr double kMinimumRealFps = 18.0;
    static constexpr double kMaximumRelativeJitter = 0.30;

    std::vector<double> intervals_ms;
    std::chrono::steady_clock::time_point last_real_frame{};
    double generation_ema_ms = 0.0;
    uint32_t cooldown_frames = 0;
    uint64_t queue_misses = 0;
    uint64_t generated_frames = 0;
    uint64_t scene_cuts = 0;
    uint64_t scene_guard_skips = 0;

    void reset() {
        intervals_ms.clear();
        last_real_frame = {};
        generation_ema_ms = 0.0;
        cooldown_frames = 0;
        queue_misses = 0;
        generated_frames = 0;
        scene_cuts = 0;
        scene_guard_skips = 0;
    }

    double observe_real_frame() {
        const auto now = std::chrono::steady_clock::now();
        if (last_real_frame.time_since_epoch().count() == 0) {
            last_real_frame = now;
            return 0.0;
        }

        const double delta_ms = std::chrono::duration<double, std::milli>(now - last_real_frame).count();
        last_real_frame = now;

        const double avg = average_ms();
        const bool discontinuity = delta_ms > 250.0
            || (avg > 0.0 && delta_ms > std::max(80.0, avg * 2.25));
        if (discontinuity) {
            intervals_ms.clear();
            cooldown_frames = std::max<uint32_t>(cooldown_frames, 8);
            return delta_ms;
        }

        if (delta_ms >= 2.0 && delta_ms <= 250.0) {
            intervals_ms.push_back(delta_ms);
            if (intervals_ms.size() > kCapacity)
                intervals_ms.erase(intervals_ms.begin());
        }
        return delta_ms;
    }

    double average_ms() const {
        if (intervals_ms.empty())
            return 0.0;
        double sum = 0.0;
        for (const double value : intervals_ms)
            sum += value;
        return sum / static_cast<double>(intervals_ms.size());
    }

    double jitter_ratio() const {
        if (intervals_ms.size() < 2)
            return 1.0;
        const double avg = average_ms();
        if (avg <= 0.0)
            return 1.0;
        double variance = 0.0;
        for (const double value : intervals_ms) {
            const double d = value - avg;
            variance += d * d;
        }
        variance /= static_cast<double>(intervals_ms.size());
        return std::sqrt(variance) / avg;
    }

    double real_fps() const {
        const double avg = average_ms();
        return avg > 0.0 ? 1000.0 / avg : 0.0;
    }

    bool eligible() {
        if (cooldown_frames > 0) {
            --cooldown_frames;
            return false;
        }
        if (intervals_ms.size() < kMinimumSamples)
            return false;
        return real_fps() >= kMinimumRealFps
            && jitter_ratio() <= kMaximumRelativeJitter;
    }

    void note_generation(const double generation_ms) {
        if (generation_ema_ms <= 0.0)
            generation_ema_ms = generation_ms;
        else
            generation_ema_ms = generation_ema_ms * 0.90 + generation_ms * 0.10;
        ++generated_frames;
    }

    void note_queue_miss() {
        ++queue_misses;
        cooldown_frames = std::max<uint32_t>(cooldown_frames, 2);
    }

    void note_scene_cut() {
        ++scene_cuts;
        ++scene_guard_skips;
        intervals_ms.clear();
        cooldown_frames = std::max<uint32_t>(cooldown_frames, 6);
    }
};

struct FrameSignature {
    static constexpr size_t kColumns = 12;
    static constexpr size_t kRows = 6;
    std::array<float, kColumns * kRows> luma{};
    bool valid = false;
};

struct FrameSignatureDelta {
    float mean_delta = 0.0f;
    float changed_fraction = 0.0f;
};

static float half_to_float(const uint16_t value) {
    const uint32_t sign = (value >> 15) & 1u;
    const uint32_t exponent = (value >> 10) & 0x1Fu;
    const uint32_t fraction = value & 0x3FFu;
    const float direction = sign ? -1.0f : 1.0f;

    if (exponent == 0) {
        if (fraction == 0)
            return sign ? -0.0f : 0.0f;
        return direction * std::ldexp(static_cast<float>(fraction) / 1024.0f, -14);
    }
    if (exponent == 0x1Fu)
        return 0.0f;

    return direction * std::ldexp(
        1.0f + static_cast<float>(fraction) / 1024.0f,
        static_cast<int>(exponent) - 15);
}

static FrameSignatureDelta compare_signatures(
    const FrameSignature& current,
    const FrameSignature& previous) {
    FrameSignatureDelta delta{};
    if (!current.valid || !previous.valid)
        return delta;

    size_t changed = 0;
    float total = 0.0f;
    for (size_t i = 0; i < current.luma.size(); ++i) {
        const float d = std::abs(current.luma[i] - previous.luma[i]);
        total += d;
        if (d >= 0.20f)
            ++changed;
    }
    delta.mean_delta = total / static_cast<float>(current.luma.size());
    delta.changed_fraction = static_cast<float>(changed)
        / static_cast<float>(current.luma.size());
    return delta;
}

static bool is_scene_cut(const FrameSignatureDelta& delta) {
    return (delta.mean_delta >= 0.24f && delta.changed_fraction >= 0.46f)
        || (delta.mean_delta >= 0.34f);
}

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

        // LSFG 3.1P Performance shader aliases.
        { "p_mipmaps", 255 },
        { "p_generate", 256 },
        { "p_gamma[0]", 280 },
        { "p_delta[0]", 280 },
        { "p_delta[5]", 281 },
        { "p_gamma[1]", 282 },
        { "p_gamma[2]", 283 },
        { "p_gamma[3]", 284 },
        { "p_gamma[4]", 285 },
        { "p_delta[1]", 286 },
        { "p_delta[2]", 287 },
        { "p_delta[3]", 288 },
        { "p_delta[4]", 289 },
        { "p_alpha[0]", 290 },
        { "p_alpha[1]", 291 },
        { "p_alpha[2]", 292 },
        { "p_alpha[3]", 293 },
        { "p_delta[6]", 294 },
        { "p_delta[7]", 295 },
        { "p_delta[8]", 296 },
        { "p_delta[9]", 297 },
        { "p_beta[0]", 298 },
        { "p_beta[1]", 299 },
        { "p_beta[2]", 300 },
        { "p_beta[3]", 301 },
        { "p_beta[4]", 302 },
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
    VkExtent2D fg_extent{};
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> swapchain_images;

    AhbImage input_0;
    AhbImage input_1;
    AhbImage output_0;

    VkCommandBuffer copy_command_buffer = VK_NULL_HANDLE;
    VkSemaphore generated_acquire = VK_NULL_HANDLE;
    VkSemaphore generated_ready = VK_NULL_HANDLE;
    VkFence copy_fence = VK_NULL_HANDLE;

    int32_t context_id = -1;
    bool library_initialized = false;
    bool session_active = false;
    bool session_failed = false;
    bool input_primed = false;
    int session_multiplier = 2;
    uint64_t frame_index = 0;
    uint64_t lsfg_frame_index = 0;
    uint32_t generation_miss_streak = 0;
    FrameTimingHistory timing;
    std::string library_cache_dir;

    static constexpr uint64_t kWarmupFrames = 60;
    static constexpr uint32_t kMissesBeforeHudPause = 2;

    bool scene_probe_supported = true;
    bool scene_probe_warned = false;

    FrameSignature capture_signature(AhbImage& image) {
        FrameSignature signature{};
        if (!scene_probe_supported || !image.ahb)
            return signature;

        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(image.ahb, &desc);
        if (desc.format != AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT
            || desc.width == 0 || desc.height == 0 || desc.stride == 0) {
            scene_probe_supported = false;
            return signature;
        }

        void* mapped = nullptr;
        const int lock_result = AHardwareBuffer_lock(
            image.ahb,
            AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
            -1,
            nullptr,
            &mapped);
        if (lock_result != 0 || !mapped) {
            scene_probe_supported = false;
            if (!scene_probe_warned) {
                scene_probe_warned = true;
                LOG_WARN("[VS-FG] visual scene-cut probe unavailable; keeping timing-only guard");
            }
            return signature;
        }

        const auto* pixels = static_cast<const uint16_t*>(mapped);
        size_t sample_index = 0;
        for (size_t row = 0; row < FrameSignature::kRows; ++row) {
            const uint32_t y = std::min<uint32_t>(
                desc.height - 1,
                static_cast<uint32_t>(((row + 1) * desc.height)
                    / (FrameSignature::kRows + 1)));
            for (size_t col = 0; col < FrameSignature::kColumns; ++col) {
                const uint32_t x = std::min<uint32_t>(
                    desc.width - 1,
                    static_cast<uint32_t>(((col + 1) * desc.width)
                        / (FrameSignature::kColumns + 1)));
                const size_t pixel_offset =
                    (static_cast<size_t>(y) * desc.stride + x) * 4u;

                const float r = std::max(0.0f, half_to_float(pixels[pixel_offset + 0]));
                const float g = std::max(0.0f, half_to_float(pixels[pixel_offset + 1]));
                const float b = std::max(0.0f, half_to_float(pixels[pixel_offset + 2]));
                const float linear_luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                signature.luma[sample_index++] =
                    linear_luma / (1.0f + linear_luma);
            }
        }

        AHardwareBuffer_unlock(image.ahb, nullptr);
        signature.valid = sample_index == signature.luma.size();
        return signature;
    }

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
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(
                device(), &semaphore_info, nullptr, &generated_ready),
            "vkCreateSemaphore(LSFG ready)");

        const VkFenceCreateInfo fence_info{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateFence(
                device(), &fence_info, nullptr, &copy_fence),
            "vkCreateFence(LSFG copy)");
    }

    void destroy_sync_objects() {
        if (copy_fence != VK_NULL_HANDLE) {
            VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyFence(device(), copy_fence, nullptr);
            copy_fence = VK_NULL_HANDLE;
        }
        if (generated_ready != VK_NULL_HANDLE) {
            VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(device(), generated_ready, nullptr);
            generated_ready = VK_NULL_HANDLE;
        }
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
            LSFG_3_1P::waitIdle();
            LSFG_3_1P::finalize();
            library_initialized = false;
            library_cache_dir.clear();
        }

        LSFG_3_1P::initialize(
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
        if (copy_fence != VK_NULL_HANDLE) {
            check_vk(
                VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences(
                    device(), 1, &copy_fence, VK_TRUE, std::numeric_limits<uint64_t>::max()),
                "vkWaitForFences(LSFG copy)");
        }

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
                { static_cast<int32_t>(fg_extent.width), static_cast<int32_t>(fg_extent.height), 1 },
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
                { static_cast<int32_t>(fg_extent.width), static_cast<int32_t>(fg_extent.height), 1 },
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

    void submit_copy_and_wait(const VkSemaphore wait) {
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
            VULKAN_HPP_DEFAULT_DISPATCHER.vkResetFences(device(), 1, &copy_fence),
            "vkResetFences(LSFG copy)");
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(queue(), 1, &submit, copy_fence),
            "vkQueueSubmit(LSFG input copy)");
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences(
                device(), 1, &copy_fence, VK_TRUE, std::numeric_limits<uint64_t>::max()),
            "vkWaitForFences(LSFG input copy)");
    }

    void submit_copy_async(const VkSemaphore wait, const VkSemaphore signal) {
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1u : 0u,
            .pWaitSemaphores = wait != VK_NULL_HANDLE ? &wait : nullptr,
            .pWaitDstStageMask = wait != VK_NULL_HANDLE ? &wait_stage : nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &copy_command_buffer,
            .signalSemaphoreCount = signal != VK_NULL_HANDLE ? 1u : 0u,
            .pSignalSemaphores = signal != VK_NULL_HANDLE ? &signal : nullptr,
        };

        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkResetFences(device(), 1, &copy_fence),
            "vkResetFences(LSFG output)");
        check_vk(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(queue(), 1, &submit, copy_fence),
            "vkQueueSubmit(LSFG output copy)");
    }

    VkResult queue_present(const uint32_t image_index, const VkSemaphore wait = VK_NULL_HANDLE) {
        const VkPresentInfoKHR present_info{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = nullptr,
            .waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1u : 0u,
            .pWaitSemaphores = wait != VK_NULL_HANDLE ? &wait : nullptr,
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
        input_primed = false;
        frame_index = 0;
        lsfg_frame_index = 0;
        generation_miss_streak = 0;
        timing.reset();
        scene_probe_supported = true;
        scene_probe_warned = false;

        if (swapchain == VK_NULL_HANDLE
            && context_id < 0
            && copy_command_buffer == VK_NULL_HANDLE) {
            return;
        }

        if (queue() != VK_NULL_HANDLE)
            VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueWaitIdle(queue());

        if (context_id >= 0) {
            try {
                LSFG_3_1P::waitIdle();
                LSFG_3_1P::deleteContext(context_id);
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
        fg_extent = {};
    }

    void shutdown() {
        release_swapchain();
        if (library_initialized) {
            try {
                LSFG_3_1P::waitIdle();
                LSFG_3_1P::finalize();
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
            set_last_error("VitaStation currently supports LSFG 2x only");
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
            fg_extent = {
                std::max(2u, (new_extent.width / 2u) & ~1u),
                std::max(2u, (new_extent.height / 2u) & ~1u)
            };
            swapchain_format = new_swapchain_format;
            swapchain_images = new_images;
            session_multiplier = 2;

            create_sync_objects();

            input_0.create(state, fg_extent, internal_format);
            input_1.create(state, fg_extent, internal_format);
            output_0.create(state, fg_extent, internal_format);

            initialize_ahb_layout(input_0);
            initialize_ahb_layout(input_1);
            initialize_ahb_layout(output_0);

            initialize_library(runtime.cache_dir);

            std::vector<AHardwareBuffer*> outputs{ output_0.ahb };
            context_id = LSFG_3_1P::createContextFromAHB(
                input_0.ahb,
                input_1.ahb,
                outputs,
                fg_extent,
                internal_format);

            session_active = true;
            session_failed = false;
            input_primed = false;
            frame_index = 0;
            lsfg_frame_index = 0;
            generation_miss_streak = 0;
            timing.reset();
            set_last_error("");
            // Runtime-active stays false during the startup warm-up. This keeps
            // the HUD from advertising doubled FPS before a generated frame is
            // actually presented.
            set_active(false);

            LOG_INFO(
                "VitaStation LSFG 3.1P 2x ready: output {}x{}, internal {}x{} on {}",
                extent.width,
                extent.height,
                fg_extent.width,
                fg_extent.height,
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

        bool render_ready_consumed = false;

        try {
            timing.observe_real_frame();

            // Do not put LSFG in the game's startup path. Shader precompile,
            // DLC/network checks and first-scene boot are timing-sensitive on
            // several Vita titles. During warm-up, present the real frame using
            // the renderer semaphore exactly like the normal Vita3K path.
            if (frame_index + 1 < kWarmupFrames) {
                set_active(false);
                ++frame_index;
                return queue_present(real_image_index, render_ready);
            }

            // Adaptive 2x: only interpolate when the real-frame cadence is
            // healthy enough. Loading screens, severe stutter and low base FPS
            // stay on real frames and re-prime after stability returns.
            if (!timing.eligible()) {
                set_active(false);
                input_primed = false;
                ++frame_index;
                return queue_present(real_image_index, render_ready);
            }

            // One frame before generation starts, capture a previous frame into
            // the opposite input image. The first LSFG call (frame 0) expects
            // input_0 to be current and input_1 to be previous.
            if (!input_primed) {
                AhbImage& previous_input =
                    (lsfg_frame_index % 2 == 0) ? input_1 : input_0;
                record_real_to_input(swapchain_images[real_image_index], previous_input);
                submit_copy_and_wait(render_ready);
                render_ready_consumed = true;
                input_primed = true;
                ++frame_index;
                set_active(false);
                return queue_present(real_image_index);
            }

            AhbImage& current_input =
                (lsfg_frame_index % 2 == 0) ? input_0 : input_1;

            record_real_to_input(swapchain_images[real_image_index], current_input);
            submit_copy_and_wait(render_ready);
            render_ready_consumed = true;

            AhbImage& previous_input =
                (lsfg_frame_index % 2 == 0) ? input_1 : input_0;
            const FrameSignature current_signature = capture_signature(current_input);
            const FrameSignature previous_signature = capture_signature(previous_input);
            const FrameSignatureDelta signature_delta =
                compare_signatures(current_signature, previous_signature);

            if (is_scene_cut(signature_delta)) {
                timing.note_scene_cut();
                input_primed = false;
                generation_miss_streak = 0;
                set_active(false);
                ++frame_index;

                LOG_INFO(
                    "[VS-FG-SCENE] cut detected: mean_delta={:.3f} changed={:.1f}% cuts={} -> history reset",
                    signature_delta.mean_delta,
                    signature_delta.changed_fraction * 100.0f,
                    timing.scene_cuts);

                return queue_present(real_image_index);
            }

            // AHardwareBuffer is shared with LSFG's own Vulkan device. We still
            // need the LSFG-side waitIdle for cross-device visibility, but the
            // VitaStation queue itself is no longer stalled with queueWaitIdle.
            const auto generation_begin = std::chrono::steady_clock::now();
            LSFG_3_1P::presentContext(context_id, -1, {});
            LSFG_3_1P::waitIdle();
            const auto generation_end = std::chrono::steady_clock::now();
            const auto generation_us = std::chrono::duration_cast<std::chrono::microseconds>(
                generation_end - generation_begin).count();
            timing.note_generation(static_cast<double>(generation_us) / 1000.0);

            ++lsfg_frame_index;

            // Never block the emulator waiting for a second swapchain image.
            // If Android's compositor has no free image right now, keep the real
            // frame and try FG again next frame instead of turning a 30 FPS game
            // into a stuttering 15 FPS game.
            uint32_t generated_index = 0;
            const VkResult acquire_result =
                VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(
                    device(),
                    swapchain,
                    0,
                    generated_acquire,
                    VK_NULL_HANDLE,
                    &generated_index);

            if (acquire_result == VK_NOT_READY || acquire_result == VK_TIMEOUT) {
                ++generation_miss_streak;
                timing.note_queue_miss();
                if (generation_miss_streak >= kMissesBeforeHudPause)
                    set_active(false);
                ++frame_index;
                return queue_present(real_image_index);
            }

            if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR)
                return acquire_result;

            generation_miss_streak = 0;

            record_output_to_swapchain(output_0, swapchain_images.at(generated_index));
            submit_copy_async(generated_acquire, generated_ready);

            VkResult generated_result = queue_present(generated_index, generated_ready);
            if (generated_result != VK_SUCCESS && generated_result != VK_SUBOPTIMAL_KHR)
                return generated_result;

            const VkResult real_result = queue_present(real_image_index);
            ++frame_index;

            if (real_result != VK_SUCCESS && real_result != VK_SUBOPTIMAL_KHR)
                return real_result;

            set_active(true, session_multiplier);

            if ((frame_index % 120u) == 0u) {
                LOG_INFO(
                    "[VS-FG] LSFG 3.1P adaptive-2x: real_fps={:.1f} jitter={:.3f} generation_ema={:.2f}ms queue_misses={} scene_cuts={} guard_skips={} output={}x{} internal={}x{}",
                    timing.real_fps(),
                    timing.jitter_ratio(),
                    timing.generation_ema_ms,
                    timing.queue_misses,
                    timing.scene_cuts,
                    timing.scene_guard_skips,
                    extent.width,
                    extent.height,
                    fg_extent.width,
                    fg_extent.height);
            }

            return generated_result == VK_SUBOPTIMAL_KHR
                ? generated_result
                : real_result;
        } catch (const std::exception& e) {
            const std::string message = std::string("LSFG runtime failed: ") + e.what();
            mark_session_failed(message);

            // If the input copy already consumed the renderer's semaphore, do
            // not wait on it again in the fallback present.
            const VkResult fallback = queue_present(
                real_image_index,
                render_ready_consumed ? VK_NULL_HANDLE : render_ready);
            return fallback == VK_SUCCESS || fallback == VK_SUBOPTIMAL_KHR
                ? fallback
                : VK_ERROR_INITIALIZATION_FAILED;
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
