cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(Tonjogram VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
# Static MSVC runtime -> a single .exe with no VC++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Dependencies (downloaded automatically at configure time)
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)  # X11 (works under XWayland too)

FetchContent_Declare(glfw
  URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(miniaudio
  URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.22.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(glfw imgui miniaudio)

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
set(SOURCES src/main.cpp src/dsp.cpp src/audio_capture.cpp)
if(APPLE)
  list(APPEND SOURCES src/mac_system_tap.mm)
  set_source_files_properties(src/mac_system_tap.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

add_executable(Tonjogram WIN32 MACOSX_BUNDLE ${SOURCES})
target_include_directories(Tonjogram PRIVATE src ${miniaudio_SOURCE_DIR})
target_link_libraries(Tonjogram PRIVATE imgui)

if(MSVC)
  target_compile_options(Tonjogram PRIVATE /O2 /fp:fast)
  # WIN32 subsystem (no console window) while keeping a standard main().
  target_link_options(Tonjogram PRIVATE /ENTRY:mainCRTStartup)
  target_compile_definitions(Tonjogram PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(Tonjogram PRIVATE -O2)
endif()

if(APPLE)
  target_link_libraries(Tonjogram PRIVATE
    "-framework Foundation" "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
  set_target_properties(Tonjogram PROPERTIES
    OUTPUT_NAME "Tonjogram"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.nicktonjum.tonjogram"
    MACOSX_BUNDLE_BUNDLE_NAME "Tonjogram"
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  # Ad-hoc sign so macOS privacy prompts (System Audio Recording / Microphone) attach to the app.
  add_custom_command(TARGET Tonjogram POST_BUILD
    COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:Tonjogram>"
    VERBATIM)
elseif(UNIX)
  target_link_libraries(Tonjogram PRIVATE ${CMAKE_DL_LIBS} pthread m)
endif()
