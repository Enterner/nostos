cmake_minimum_required(VERSION 3.24)

# ── nostos 库本体 ───────────────────────────────────────────────────────────
#
# 这个目录是库：header-only 核心 + 唯一需要编译的 loader + 安装/导出规则。
# 应用（app/）与插件（plugin/）都在上层工作区，不属于这个包。
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
    # /permissive- ：核心用到 C++20 concepts 与类类型 NTTP。
    # /EHsc        ：强制项，不是风格偏好。CMake 会把 MSBuild 的
    #                <ExceptionHandling> 留空，缺了它 cl.exe 回退到不合规的
    #                异常模型：/O2 下异常穿越 try 块时不再析构局部对象。
    #                nostos 的全部契约是"异常 ⇒ scope unwind、副作用回滚"，
    #                不开 /EHsc 的构建会静默破坏本库（Debug /Od 碰巧能跑，
    #                Release 不行）。
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
        # 与 MSVC 分支同义：仅 ASan，保证 preset 在各编译器上含义一致。
        # 需要时再追加 ,undefined（UBSan）或改用 -fsanitize=thread 跑 TSan。
        target_compile_options(nostos INTERFACE
            $<BUILD_INTERFACE:-fsanitize=address -fno-omit-frame-pointer>)
        target_link_options(nostos INTERFACE $<BUILD_INTERFACE:-fsanitize=address>)
    endif()
endif()

# ---- loader：库里唯一需要编译的部分 ------------------------------------------
#
# L2、且刻意可选：header-only 核心从不需要它；它把 <windows.h>/<dlfcn.h> 挡在
# 每个仅加载插件的翻译单元之外。

add_library(nostos_loader STATIC src/loader/loader.cpp)
add_library(nostos::loader ALIAS nostos_loader)
target_link_libraries(nostos_loader PUBLIC nostos::core)
# 安装名是 nostos::loader（alias 不会被导出，导出的是 EXPORT_NAME）。
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

# ---- kit（Tier 1 能力层）----------------------------------------------------
#
# 头随 core 分发（include/nostos/kit/）；config 的 JSON 层使用 vendored
# nlohmann/json（third_party/nlohmann/json.hpp，3.11.3，版本钉死）。
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
# 头注释。
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
