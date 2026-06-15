/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    Android backend, 2026
 *
 * Android entry point. The activity's NativeActivity loads this library and
 * android_native_app_glue calls android_main on a dedicated thread (which
 * becomes the UI thread). Mirrors windowed_app_main_posix.cpp.
 */

#include <android/log.h>
#include <android_native_app_glue.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform_android_jni.h>
#include <rex/thread.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_android.h>

extern "C" void android_main(struct android_app* app) {

  // Load dynamically-resolved libandroid/libc symbols before anything uses them.
  // Critically, memory::AndroidInitialize() resolves ASharedMemory_create, which
  // backs the guest 4 GiB mapping; without it CreateFileMappingHandle falls back
  // to raw /dev/ashmem, which is blocked for apps targeting API 29+ and fails on
  // the emulator and stricter devices ("Unable to reserve the 4gb guest address
  // space"). thread::AndroidInitialize() resolves pthread_getname_np.
  rex::memory::AndroidInitialize();
  rex::thread::AndroidInitialize();

  // Publish the JavaVM + activity (Context) for JNI-backed features (content://
  // file descriptors, controller rumble).
  rex::SetAndroidRuntimeJni(app->activity->vm, app->activity->clazz);

  // No process command line on Android. Synthesise argv: program name plus, when
  // available, --game_data_root pointing at the app's external files dir where
  // the game files (default.xex, music/sfx banks) are staged. The runtime mounts
  // that dir as the guest game:/d: volume (Runtime::SetupVfs). Other launch
  // options come from the in-app menu / on-disk config, as on desktop.
  // All runtime-writable paths must live under a writable dir; the defaults are
  // derived from the (read-only) executable folder /system/bin and abort. Point
  // game data, logs, the shader cache and user data at the app's external files.
  std::string game_root_arg, log_file_arg, cache_arg, user_arg;
  if (app->activity->externalDataPath && app->activity->externalDataPath[0]) {
    const std::string ext = app->activity->externalDataPath;
    game_root_arg = "--game_data_root=" + ext;
    log_file_arg = "--log_file=" + ext + "/ge.log";
    cache_arg = "--cache_path=" + ext + "/cache";
    user_arg = "--user_data_root=" + ext + "/user";
  }
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("ge"));
  for (std::string* a : {&game_root_arg, &log_file_arg, &cache_arg, &user_arg}) {
    if (!a->empty()) {
      argv.push_back(const_cast<char*>(a->c_str()));
    }
  }
  rex::cvar::Init(static_cast<int>(argv.size()), argv.data());
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  {
    rex::ui::AndroidWindowedAppContext app_context(app);

    rex::ui::WindowedApp::Creator creator = rex::ui::WindowedApp::GetAnyCreator();
    if (!creator) {
      __android_log_print(ANDROID_LOG_ERROR, "rex",
                          "No WindowedApp registered in this library");
    } else {
      std::unique_ptr<rex::ui::WindowedApp> windowed_app = creator(app_context);
      if (windowed_app->OnInitialize()) {
        app_context.RunMainAndroidLoop();
      }
      windowed_app->InvokeOnDestroy();
    }
  }

  rex::ShutdownLogging();
}
