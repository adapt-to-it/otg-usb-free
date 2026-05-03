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

# shaderc viene fornito dal NDK come modulo prefab. Il consumer deve avere
# `buildFeatures { prefab = true }` in build.gradle. Se non disponibile
# (NDK molto vecchio) la build fallisce con un messaggio chiaro.
find_package(shaderc REQUIRED CONFIG)

target_link_libraries(otgcam_native
    PRIVATE
        ${LIB_VULKAN}
        ${LIB_ANDROID}
        ${LIB_LOG}
        shaderc::shaderc
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
