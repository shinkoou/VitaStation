#pragma once
#include <string>

namespace vitastation::lsfg {

enum class PrepareResult : int {
    Ok = 0,
    DllUnreadable = -1,
    MissingShaders = -2,
    TranslationFailed = -3,
    WriteFailed = -4,
};

int prepare_shaders(const std::string& dll_path, const std::string& cache_dir);
bool shaders_ready(const std::string& cache_dir);
const char* backend_info();

} // namespace vitastation::lsfg
