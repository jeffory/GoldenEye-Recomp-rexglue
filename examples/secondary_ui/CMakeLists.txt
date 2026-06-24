# Desktop harness proving rex::ui::SecondaryUiSurface (dual-screen foundation).
# Built only when -DREXGLUE_BUILD_SECONDARY_UI_DEMO=ON. Currently provides the
# GNU/Linux (xcb) path; the source is structured so Win32 can be added similarly.

add_library(rexui_secondary_demo STATIC
    secondary_ui_demo.cpp
)

target_include_directories(rexui_secondary_demo PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rexui_secondary_demo PUBLIC
    rex::ui
    imgui::imgui
)

if(NOT WIN32 AND NOT ANDROID)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    target_include_directories(rexui_secondary_demo PRIVATE ${X11_XCB_INCLUDE_DIRS})
    target_link_libraries(rexui_secondary_demo PUBLIC ${X11_XCB_LIBRARIES})
endif()
