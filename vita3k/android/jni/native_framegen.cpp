#include "../lsfg/shader_store.h"

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
