#include "shader_store.h"

#include <extract/trans.hpp>
#include <pe-parse/parse.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace vitastation::lsfg {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kDxbcFirst = 255;
constexpr uint32_t kDxbcLast = 302;
constexpr uint32_t kFp16First = 304;
constexpr uint32_t kFp16Last = 351;
constexpr uint32_t kFp32First = 353;
constexpr uint32_t kFp32Last = 400;

using ResourceMap = std::unordered_map<uint32_t, std::vector<uint8_t>>;

bool is_spirv(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < sizeof(uint32_t))
        return false;
    uint32_t magic = 0;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    return magic == kSpirvMagic;
}

bool write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

int collect_resource(void* user, const peparse::resource& res) {
    auto* resources = static_cast<ResourceMap*>(user);
    if (!resources || res.type != peparse::RT_RCDATA ||
        res.buf == nullptr || res.buf->bufLen <= 0) {
        return 0;
    }

    std::vector<uint8_t> bytes(res.buf->bufLen);
    std::copy_n(res.buf->buf, res.buf->bufLen, bytes.data());
    (*resources)[res.name] = std::move(bytes);
    return 0;
}

bool required_dxbc_present(const ResourceMap& resources) {
    for (uint32_t id = kDxbcFirst; id <= kDxbcLast; ++id) {
        if (resources.find(id) == resources.end())
            return false;
    }
    return true;
}

bool cached_range_ready(const fs::path& base, uint32_t first, uint32_t last) {
    for (uint32_t id = first; id <= last; ++id) {
        const fs::path file = base / (std::to_string(id) + ".spv");
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
            return false;
        uint32_t magic = 0;
        stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (!stream || magic != kSpirvMagic)
            return false;
    }
    return true;
}

void write_optional_spirv_range(const ResourceMap& resources,
                                const fs::path& directory,
                                uint32_t first,
                                uint32_t last) {
    for (uint32_t id = first; id <= last; ++id) {
        const auto it = resources.find(id);
        if (it == resources.end() || !is_spirv(it->second))
            return;
    }

    fs::create_directories(directory);
    for (uint32_t id = first; id <= last; ++id) {
        if (!write_file(directory / (std::to_string(id) + ".spv"), resources.at(id)))
            throw std::runtime_error("failed to write optional SPIR-V cache");
    }
}

} // namespace

int prepare_shaders(const std::string& dll_path, const std::string& cache_dir) {
    try {
        peparse::parsed_pe* dll = peparse::ParsePEFromFile(dll_path.c_str());
        if (!dll)
            return static_cast<int>(PrepareResult::DllUnreadable);

        ResourceMap resources;
        peparse::IterRsrc(dll, collect_resource, &resources);
        peparse::DestructParsedPE(dll);

        if (!required_dxbc_present(resources))
            return static_cast<int>(PrepareResult::MissingShaders);

        const fs::path destination(cache_dir);
        const fs::path staging = destination.string() + ".staging";

        std::error_code ec;
        fs::remove_all(staging, ec);
        fs::create_directories(staging);

        for (uint32_t id = kDxbcFirst; id <= kDxbcLast; ++id) {
            std::vector<uint8_t> spirv;
            try {
                spirv = Extract::translateShader(resources.at(id));
            } catch (...) {
                fs::remove_all(staging, ec);
                return static_cast<int>(PrepareResult::TranslationFailed);
            }

            if (!is_spirv(spirv) ||
                !write_file(staging / (std::to_string(id) + ".spv"), spirv)) {
                fs::remove_all(staging, ec);
                return static_cast<int>(PrepareResult::WriteFailed);
            }
        }

        write_optional_spirv_range(resources, staging / "fp16", kFp16First, kFp16Last);
        write_optional_spirv_range(resources, staging / "fp32", kFp32First, kFp32Last);

        fs::remove_all(destination, ec);
        fs::rename(staging, destination, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            return static_cast<int>(PrepareResult::WriteFailed);
        }

        return shaders_ready(cache_dir)
            ? static_cast<int>(PrepareResult::Ok)
            : static_cast<int>(PrepareResult::WriteFailed);
    } catch (...) {
        return static_cast<int>(PrepareResult::WriteFailed);
    }
}

bool shaders_ready(const std::string& cache_dir) {
    return cached_range_ready(fs::path(cache_dir), kDxbcFirst, kDxbcLast);
}

const char* backend_info() {
    return "lsfg-vk-android 1.0.0 / LSFG 3.1P Performance / Vulkan AHardwareBuffer";
}

} // namespace vitastation::lsfg
