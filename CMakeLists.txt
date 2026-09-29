cmake_minimum_required(VERSION 3.24)

# ── nostos 库本体 ───────────────────────────────────────────────────────────
#
# 这个目录**只有库**：header-only 核心 + 唯一需要编译的 loader + 安装/导出规则。
# 应用（app/）、插件（plugin/）、示例与测试都在上层工作区，不属于这个包。
#
# 两种用法：
#   1) 被工作区包含：上层 CMakeLists 里 add_subdirectory(nostos)
#   2) 独立配置：   cmake -S nostos -B build && cmake --install build --prefix <prefix>

# 独立配置时才自建 project（被 add_subdirectory 时沿用上层的工作区项目）。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nostos VERSION 0.1.0 LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_C_STANDARD 11)
endif()

set(nostos_VERSION 0.1.0)

# ---- 核心：header-only，零依赖 ----------------------------------------------

add_library(nostos INTERFACE)
add_library(nostos::core ALIAS nostos)
set_target_properties(nostos PROPERTIES EXPORT_NAME core)
target_include_directories(nostos INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(nostos INTERFACE cxx_std_20)

if(MSVC)
    # /permissive- : the core uses C++20 concepts and class-type NTTPs.
    # /EHsc        : MANDATORY, not a style choice. CMake leaves MSBuild's
    #                <ExceptionHandling> empty, so without this cl.exe falls
    #                back to a non-conforming exception model: at /O2 it stops
    #                destroying locals while an exception propagates out of a
    #                try block. nostos's whole contract is "exception =>
    #                scope unwinds and side effects roll back", so a build
    #                without /EHsc silently breaks the library (Debug /Od
    #                happens to work, Release does not).
    target_compile_options(nostos INTERFACE /permissive- /EHsc)
endif()

# ---- 消毒器 ------------------------------------------------------------------
#
# ASan 是"这次构建"的要求，而不是某个目标的要求，所以挂成 nostos 目标的 INTERFACE 选项：
# 任何链接 nostos::core 的目标（loader、应用、插件、测试）都自动跟着插桩。
#
# 关键是 $<BUILD_INTERFACE:...>：这些标志**不会**进入安装后的导入目标 —— 否则第三方
# 消费者会被迫也开 ASan（而且会撞 LNK2038）。库分发出去的接口保持干净。

option(NOSTOS_ASAN "Build with AddressSanitizer (cmake --preset asan)" OFF)

if(NOSTOS_ASAN)
    if(MSVC)
        target_compile_options(nostos INTERFACE $<BUILD_INTERFACE:/fsanitize=address>)
    else()
        # Same meaning as the MSVC branch: ASan only, so the preset means one
        # thing on every compiler. Add ,undefined (UBSan) or switch to -fsanitize=thread
        # for a TSan run — the design notes call for all three; neither is
        # exercised by the presets because neither was available locally.
        target_compile_options(nostos INTERFACE
            $<BUILD_INTERFACE:-fsanitize=address -fno-omit-frame-pointer>)
        target_link_options(nostos INTERFACE $<BUILD_INTERFACE:-fsanitize=address>)
    endif()
endif()

# ---- loader：库里唯一需要编译的部分 ------------------------------------------
#
# L2 and optional by design: the header-only core never needs it, and it keeps
# <windows.h>/<dlfcn.h> out of every translation unit that merely loads a plugin.

add_library(nostos_loader STATIC src/loader/loader.cpp)
add_library(nostos::loader ALIAS nostos_loader)
target_link_libraries(nostos_loader PUBLIC nostos::core)
# The installed name is nostos::loader (aliases are not exported; EXPORT_NAME is).
set_target_properties(nostos_loader PROPERTIES EXPORT_NAME loader)

# ---- 安装与导出 ---------------------------------------------------------------
#
#   cmake -S nostos -B build && cmake --install build --prefix <prefix>
#   find_package(nostos CONFIG REQUIRED) → nostos::core / nostos::loader

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS nostos nostos_loader
    EXPORT nostosTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/nostos
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT nostosTargets
    NAMESPACE nostos::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nostos)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/nostosConfigVersion.cmake"
    VERSION ${nostos_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/nostosConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/nostosConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nostos)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/nostosConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/nostosConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nostos)

# ---- kit（Tier 1 能力层，docs/proposal-kit.md §5）----------------------------
#
# 头随 core 分发（include/nostos/kit/）；config 的 JSON 层使用 vendored
# nlohmann/json（third_party/nlohmann/json.hpp，版本见 DEPENDENCIES.md）。
# 消费方：target_link_libraries(x PRIVATE nostos::kit) 拿到全部 include 目录。

add_library(nostos_kit INTERFACE)
get_filename_component(NOSTOS_THIRD_PARTY_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party" ABSOLUTE)
target_include_directories(nostos_kit INTERFACE
    $<BUILD_INTERFACE:${NOSTOS_THIRD_PARTY_DIR}>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(nostos_kit INTERFACE nostos)
add_library(nostos::kit ALIAS nostos_kit)

install(TARGETS nostos_kit EXPORT nostosTargets)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nlohmann)

# ---- kit_http（Tier 1 唯一的编译型组件：平台 socket 代码）--------------------
#
# 单线程 select 循环 + 每客户端有界缓冲（背压闸门）——设计理由见 kit/http.hpp
# 头注释。提取自 video_stream 的 http_local，行为逐字节保留。
add_library(nostos_http STATIC src/kit/http.cpp)
target_include_directories(nostos_http PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(nostos_http PUBLIC nostos)
if(WIN32)
    target_link_libraries(nostos_http PUBLIC ws2_32)
endif()
set_target_properties(nostos_http PROPERTIES EXPORT_NAME kit_http)
add_library(nostos::kit_http ALIAS nostos_http)

install(TARGETS nostos_http EXPORT nostosTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
