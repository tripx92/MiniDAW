#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "AudioEngine.h"

namespace {
std::unique_ptr<AudioEngine> g_engine;
std::mutex g_lifecycle;   // guards create/destroy only (never touched by the audio thread)
}

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_minidaw_app_NativeEngine_##name

JNI(void, nativeCreate)(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (!g_engine) g_engine = std::make_unique<AudioEngine>();
}

JNI(jboolean, nativeStart)(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    return g_engine && g_engine->start() ? JNI_TRUE : JNI_FALSE;
}

JNI(void, nativeStop)(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (g_engine) g_engine->stop();
}

JNI(void, nativeDestroy)(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    g_engine.reset();
}

JNI(void, nativePlay)(JNIEnv*, jobject) { if (g_engine) g_engine->play(); }
JNI(void, nativeStopTransport)(JNIEnv*, jobject) { if (g_engine) g_engine->stopTransport(); }
JNI(void, nativeSlotPress)(JNIEnv*, jobject, jint t, jint s) { if (g_engine) g_engine->slotPress(t, s); }
JNI(void, nativeLaunchScene)(JNIEnv*, jobject, jint s) { if (g_engine) g_engine->launchScene(s); }
JNI(void, nativeStopTrack)(JNIEnv*, jobject, jint t) { if (g_engine) g_engine->stopTrack(t); }
JNI(void, nativeStopAll)(JNIEnv*, jobject) { if (g_engine) g_engine->stopAll(); }
JNI(void, nativeSetArm)(JNIEnv*, jobject, jint t, jboolean a) { if (g_engine) g_engine->setArm(t, a == JNI_TRUE); }
JNI(void, nativeSetGlobal)(JNIEnv*, jobject, jint id, jfloat v) { if (g_engine) g_engine->setGlobal(id, v); }
JNI(void, nativeSetTrackParam)(JNIEnv*, jobject, jint t, jint id, jfloat v) {
    if (g_engine) g_engine->setTrackParam(t, id, v);
}
JNI(void, nativeSetClipParams)(JNIEnv*, jobject, jint t, jint s, jint warp, jint bars, jint transpose) {
    if (g_engine) g_engine->setClipParams(t, s, warp, bars, transpose);
}

JNI(jboolean, nativeLoadClip)(JNIEnv* env, jobject, jint t, jint s, jfloatArray data, jint frames,
                              jstring name) {
    if (!g_engine || !data) return JNI_FALSE;
    if (env->GetArrayLength(data) < frames * 2) return JNI_FALSE;
    const char* cname = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
    jfloat* p = env->GetFloatArrayElements(data, nullptr);
    const bool ok = g_engine->loadClip(t, s, p, frames, cname ? cname : "Clip");
    env->ReleaseFloatArrayElements(data, p, JNI_ABORT);
    if (cname) env->ReleaseStringUTFChars(name, cname);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNI(void, nativeGetStatus)(JNIEnv* env, jobject, jfloatArray out) {
    if (!g_engine || !out || env->GetArrayLength(out) < kStatusSize) return;
    float tmp[kStatusSize];
    g_engine->getStatus(tmp);
    env->SetFloatArrayRegion(out, 0, kStatusSize, tmp);
}

JNI(void, nativeGetMeters)(JNIEnv* env, jobject, jfloatArray out) {
    if (!g_engine || !out || env->GetArrayLength(out) < kMeterValues) return;
    float tmp[kMeterValues];
    g_engine->getMeters(tmp);
    env->SetFloatArrayRegion(out, 0, kMeterValues, tmp);
}

JNI(jboolean, nativePollEvent)(JNIEnv* env, jobject, jintArray out) {
    if (!g_engine || !out || env->GetArrayLength(out) < 5) return JNI_FALSE;
    int tmp[5] = {0, 0, 0, 0, 0};
    if (!g_engine->pollEvent(tmp)) return JNI_FALSE;
    env->SetIntArrayRegion(out, 0, 5, tmp);
    return JNI_TRUE;
}

JNI(jstring, nativeClipName)(JNIEnv* env, jobject, jint t, jint s) {
    const std::string n = g_engine ? g_engine->clipName(t, s) : std::string();
    return env->NewStringUTF(n.c_str());
}

JNI(void, nativeClipInfo)(JNIEnv* env, jobject, jint t, jint s, jintArray out) {
    if (!g_engine || !out || env->GetArrayLength(out) < 5) return;
    int tmp[5] = {0, 0, 0, 0, 0};
    g_engine->clipInfo(t, s, tmp);
    env->SetIntArrayRegion(out, 0, 5, tmp);
}
