#include "../lsfg/shader_store.h"

#include <renderer/vulkan/frame_generation.h>

#include <jni.h>
#include <string>

namespace {

std::string to_string(JNIEnv* env, jstring value) {
    if (!value)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(value, chars);
    return result;
}

} // namespace

extern "C" JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_NativeLib_prepareLsfgShaders(
        JNIEnv* env, jclass, jstring dll_path, jstring cache_dir) {
    return vitastation::lsfg::prepare_shaders(
        to_string(env, dll_path),
        to_string(env, cache_dir));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_areLsfgShadersReady(
        JNIEnv* env, jclass, jstring cache_dir) {
    return vitastation::lsfg::shaders_ready(to_string(env, cache_dir))
        ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_getLsfgBackendInfo(JNIEnv* env, jclass) {
    return env->NewStringUTF(vitastation::lsfg::backend_info());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_configureLsfgFrameGeneration(
        JNIEnv* env, jclass, jboolean enabled, jint multiplier, jstring cache_dir) {
    const std::string cache = to_string(env, cache_dir);

    if (enabled == JNI_TRUE && !vitastation::lsfg::shaders_ready(cache))
        return JNI_FALSE;

    return renderer::vulkan::configure_frame_generation_runtime(
        enabled == JNI_TRUE,
        static_cast<int>(multiplier),
        cache)
        ? JNI_TRUE
        : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_isLsfgFrameGenerationActive(JNIEnv*, jclass) {
    return renderer::vulkan::frame_generation_active() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_NativeLib_getLsfgFrameGenerationMultiplier(JNIEnv*, jclass) {
    return static_cast<jint>(renderer::vulkan::frame_generation_active_multiplier());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_getLsfgFrameGenerationLastError(JNIEnv* env, jclass) {
    const std::string error = renderer::vulkan::frame_generation_last_error();
    return env->NewStringUTF(error.c_str());
}
