// JNI shim for the ALR in-app PulseAudio sink (design §5f, Option A).
//
// Three externals mirror the shape of MainActivity's nativeWaylandCompositor*
// trio: Kotlin only starts/stops the sink and reads its status — NO PCM crosses
// JNI (the native AlrPulseServer owns the AAudio stream directly). Registered in
// the same alr_loader .so MainActivity already loads (System.loadLibrary).
//
// HOST-ONLY, public NDK/JNI API only, W^X-safe. No SELinux interaction.

#include "alr_pulse_server.h"

#include <jni.h>
#include <string>

namespace {

// Local jstring->std::string (UTF-8). Self-contained so this TU has no dependency
// on the (non-exported) helper in runtime_report.cpp.
std::string jstr(JNIEnv* env, jstring s) {
    if (s == nullptr) return std::string();
    const char* utf = env->GetStringUTFChars(s, nullptr);
    std::string out(utf ? utf : "");
    if (utf) env->ReleaseStringUTFChars(s, utf);
    return out;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAudioSinkStart(
    JNIEnv* env, jobject /*thiz*/, jstring xdg_runtime_dir, jint device_rate) {
    const std::string dir = jstr(env, xdg_runtime_dir);
    const std::string status =
        alr::audio::alr_audio_sink_start(dir, static_cast<int>(device_rate));
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAudioSinkStatus(
    JNIEnv* env, jobject /*thiz*/) {
    return env->NewStringUTF(alr::audio::alr_audio_sink_status().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAudioSinkStop(
    JNIEnv* env, jobject /*thiz*/) {
    return env->NewStringUTF(alr::audio::alr_audio_sink_stop().c_str());
}
