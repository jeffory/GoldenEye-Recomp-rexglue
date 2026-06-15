/**
 ******************************************************************************
 * ReXGlue runtime - Android JNI bridge
 ******************************************************************************
 * @author  Android backend, 2026
 */

#include <rex/platform_android_jni.h>

#if REX_PLATFORM_ANDROID

namespace rex {

namespace {
JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;  // global ref
}  // namespace

JNIEnv* GetAndroidJniEnv() {
  if (!g_vm) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  jint status = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (status == JNI_EDETACHED) {
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      return nullptr;
    }
  } else if (status != JNI_OK) {
    return nullptr;
  }
  return env;
}

void SetAndroidRuntimeJni(JavaVM* vm, jobject activity) {
  g_vm = vm;
  if (g_vm && activity) {
    if (JNIEnv* env = GetAndroidJniEnv()) {
      g_activity = env->NewGlobalRef(activity);
    }
  }
}

JavaVM* GetAndroidJavaVM() { return g_vm; }

jobject GetAndroidActivity() { return g_activity; }

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
