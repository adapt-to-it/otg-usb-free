cmake_minimum_required(VERSION 3.22.1)

project(otgcam_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# Sorgenti
# ----------------------------------------------------------------------------
set(OTGCAM_SOURCES
    jni_bridge.cpp
    vulkan_context.cpp
    vulkan_swapchain.cpp
    preview_renderer.cpp
    frame_uploader.cpp
)

add_library(otgcam_native SHARED ${OTGCAM_SOURCES})

target_include_directories(otgcam_native
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ----------------------------------------------------------------------------
# Dipendenze NDK
#   - Vulkan: header + loader (libvulkan.so) sono parte dell'NDK.
#   - android: ANativeWindow / ALooper.
#   - log: __android_log_print.
#   - shaderc: compilazione runtime degli shader GLSL -> SPIR-V (NDK prefab).
# ----------------------------------------------------------------------------
find_library(LIB_VULKAN vulkan)
find_library(LIB_ANDROID android)
find_library(LIB_LOG log)

# shaderc viene buildato dai sorgenti scaricati in third_party/shaderc.
# NDK r27+ non distribuisce piu shaderc come prefab built-in.
set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/shaderc shaderc EXCLUDE_FROM_ALL)

target_link_libraries(otgcam_native
    PRIVATE
        ${LIB_VULKAN}
        ${LIB_ANDROID}
        ${LIB_LOG}
        shaderc
)

target_compile_options(otgcam_native
    PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O2>
)

# Compile-time flag che il bridge JNI usa per attivare verbose logging.
target_compile_definitions(otgcam_native
    PRIVATE
        $<$<CONFIG:Debug>:OTGCAM_DEBUG=1>
        $<$<CONFIG:RelWithDebInfo>:OTGCAM_DEBUG=1>
)
