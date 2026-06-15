/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>

#include <rex/filesystem.h>
#include <rex/platform.h>

#if REX_PLATFORM_ANDROID
#include <jni.h>

#include <string>

#include <rex/platform_android_jni.h>
#endif

namespace rex {
namespace filesystem {

bool CreateParentFolder(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    auto parent_path = path.parent_path();
    if (!std::filesystem::exists(parent_path)) {
      return std::filesystem::create_directories(parent_path);
    }
  }
  return true;
}

#if REX_PLATFORM_ANDROID
// Resolve a content:// URI to an owned file descriptor via JNI:
//   activity.getContentResolver().openFileDescriptor(Uri.parse(uri), mode).detachFd()
// Fully defensive - any missing method / Java exception returns -1 (callers,
// e.g. mapped_memory_posix.cpp, handle the failure). Returns -1 when the JNI
// bridge is unset (e.g. a host/tool build). Only used for content:// inputs;
// regular file paths go through the normal filesystem layer.
int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  JNIEnv* env = rex::GetAndroidJniEnv();
  jobject activity = rex::GetAndroidActivity();
  if (!env || !activity) {
    return -1;
  }
  if (env->PushLocalFrame(16) != 0) {
    return -1;
  }
  int result = -1;
  do {
    jclass activity_cls = env->GetObjectClass(activity);
    jmethodID m_get_cr = env->GetMethodID(activity_cls, "getContentResolver",
                                          "()Landroid/content/ContentResolver;");
    if (!m_get_cr) {
      break;
    }
    jobject cr = env->CallObjectMethod(activity, m_get_cr);
    if (!cr || env->ExceptionCheck()) {
      break;
    }

    jclass uri_cls = env->FindClass("android/net/Uri");
    if (!uri_cls) {
      break;
    }
    jmethodID m_parse = env->GetStaticMethodID(uri_cls, "parse",
                                               "(Ljava/lang/String;)Landroid/net/Uri;");
    if (!m_parse) {
      break;
    }
    std::string uri_str(uri);
    jstring j_uri = env->NewStringUTF(uri_str.c_str());
    jobject u = env->CallStaticObjectMethod(uri_cls, m_parse, j_uri);
    if (!u || env->ExceptionCheck()) {
      break;
    }

    jclass cr_cls = env->GetObjectClass(cr);
    jmethodID m_open = env->GetMethodID(
        cr_cls, "openFileDescriptor",
        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
    if (!m_open) {
      break;
    }
    jstring j_mode = env->NewStringUTF(mode ? mode : "r");
    jobject pfd = env->CallObjectMethod(cr, m_open, u, j_mode);
    if (!pfd || env->ExceptionCheck()) {
      break;
    }

    jclass pfd_cls = env->GetObjectClass(pfd);
    jmethodID m_detach = env->GetMethodID(pfd_cls, "detachFd", "()I");
    if (!m_detach) {
      break;
    }
    jint fd = env->CallIntMethod(pfd, m_detach);  // transfers ownership of the fd
    if (env->ExceptionCheck()) {
      break;
    }
    result = static_cast<int>(fd);
  } while (false);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    result = -1;
  }
  env->PopLocalFrame(nullptr);
  return result;
}
#endif  // REX_PLATFORM_ANDROID

}  // namespace filesystem
}  // namespace rex
