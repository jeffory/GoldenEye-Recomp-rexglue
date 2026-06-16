# rexaudio - Audio subsystem library
# Audio processing and XMA decoding for Xbox 360 emulation

add_library(rexaudio OBJECT
    
    audio_driver.cpp
    audio_system.cpp
    xma_context.cpp
    xma_decoder.cpp
    xma_register_file.cpp
    # NOP backend (always available)
    nop/nop_audio_system.cpp
    # SDL backend
    sdl/sdl_audio_system.cpp
    sdl/sdl_audio_driver.cpp
)
add_library(rex::audio ALIAS rexaudio)

if(ANDROID)
    # Native Android backend (no SDL/JNI dependency; SDL audio cannot init under
    # a bare NativeActivity).
    target_sources(rexaudio PRIVATE
        android/android_audio_system.cpp
        android/android_audio_driver.cpp
    )
    # Real low-latency output uses AAudio (NDK API 26+; build targets android-29).
    target_link_libraries(rexaudio PRIVATE aaudio)
endif()

target_include_directories(rexaudio PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(rexaudio
    PUBLIC rexcore SDL3::SDL3
    PRIVATE libavcodec libavutil
)
