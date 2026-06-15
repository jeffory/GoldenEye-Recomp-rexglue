#pragma once
/**
 ******************************************************************************
 * ReXGlue runtime - Android JNI bridge
 ******************************************************************************
 * @author  Android backend, 2026
 *
 * Small global bridge to the activity's JavaVM + Context, set once from
 * android_main. Lets low-level code reach Java APIs that have no NDK equivalent
 * (content:// file descriptors, the vibrator). Safe to call from any thread.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID
#include <jni.h>

namespace rex {

// Set once from android_main with the activity's JavaVM and the NativeActivity
// object (which is a Context). Stores a global ref to the activity.
void SetAndroidRuntimeJni(JavaVM* vm, jobject activity);

JavaVM* GetAndroidJavaVM();
// Global ref to the activity (a Context); valid for the activity lifetime, or
// null if SetAndroidRuntimeJni has not run.
jobject GetAndroidActivity();
// JNIEnv for the calling thread, attaching it to the VM if necessary. Returns
// null if the VM is unset or attach fails. Do not cache across threads.
JNIEnv* GetAndroidJniEnv();

}  // namespace rex
#endif  // REX_PLATFORM_ANDROID
