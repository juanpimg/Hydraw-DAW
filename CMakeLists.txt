cmake_minimum_required(VERSION 3.16)
project(HydrawDAW VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -ffast-math -flto -Wno-stringop-overflow")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
endif()

file(DOWNLOAD
    https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h
    ${CMAKE_SOURCE_DIR}/extern/miniaudio.h
    STATUS miniaudio_status
)
if(NOT miniaudio_status EQUAL 0)
    message(FATAL_ERROR "Failed to download miniaudio.h")
endif()

include(FetchContent)
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview
    GIT_TAG 0.12.0
)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_CHECKS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(webview)

add_executable(hydraw_daw
    src/main.cpp
    src/AudioEngine.cpp
    src/Plugin/PluginChain.cpp
    src/Project/ProjectSerializer.cpp
)

target_compile_definitions(hydraw_daw PRIVATE HYDRAW_VERSION="${PROJECT_VERSION}")

target_include_directories(hydraw_daw PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern
    ${CMAKE_SOURCE_DIR}/extern/clap
)

set_source_files_properties(src/AudioEngine.cpp PROPERTIES COMPILE_DEFINITIONS MINIAUDIO_IMPLEMENTATION)

target_link_libraries(hydraw_daw PRIVATE
    webview::core
    pthread
    dl
    rt
    GL
    X11
)

if(UNIX AND NOT APPLE)
    target_link_libraries(hydraw_daw PRIVATE asound)
endif()

# Ship bundled CLAP plugins next to the binary so the audio engine can
# preload them via a relative path. The runtime expects "plugins/<x>.clap"
# relative to the CWD; copying the dir into $<TARGET_FILE_DIR> keeps that
# invariant when launching directly from the build tree. For installed
# builds, packagers should ship the plugins/ directory alongside the
# binary (or update the runtime search path).
if(EXISTS "${CMAKE_SOURCE_DIR}/plugins")
    add_custom_command(TARGET hydraw_daw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:hydraw_daw>/plugins
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/plugins
            $<TARGET_FILE_DIR:hydraw_daw>/plugins
    )
endif()
