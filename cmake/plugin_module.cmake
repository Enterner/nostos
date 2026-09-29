# ── 编一个插件模块（MODULE）────────────────────────────────────────────────────
#
# 每个插件都是**独立工程**：自己的 CMakeLists.txt、自己的 CMakePresets.json、自己的
# build-<preset>/。这个文件只是把"模块该怎么编"这件事写一遍，供各插件 include ——
# 它不做任何编排，也不依赖别的插件。
#
# 用法（插件自己的 CMakeLists.txt 里）：
#
#     set(NOSTOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "nostos 仓库根")
#     if(NOT TARGET nostos::core)
#         add_subdirectory("${NOSTOS_ROOT}/nostos" "${CMAKE_CURRENT_BINARY_DIR}/_nostos"
#                          EXCLUDE_FROM_ALL)
#     endif()
#     include("${NOSTOS_ROOT}/nostos/cmake/plugin_module.cmake")
#     nostos_plugin_module(my_plugin plugin.cpp)
#     target_include_directories(my_plugin PRIVATE "${NOSTOS_APP_DIR}" ...)
#
# 想加一个插件：复制一个插件目录（CMakeLists + CMakePresets 都有了），改名字即可。

# 宿主契约头（app/contract.hpp）与"参考宿主"的位置。演示插件实现的是宿主的契约，所以默认
# 指向仓库根下的 app/；插件若想自带宿主（一体编译），也用这两个变量找宿主源码。
# 不实现宿主契约的插件（例如纯测试替身）用不到它们。
set(NOSTOS_APP_DIR "${NOSTOS_ROOT}/app" CACHE PATH "参考宿主目录（contract.hpp 与 main.cpp）")

# 模块产物一律落在**本工程自己的构建树**里（各自的编译在各自目录）：
#     plugin/<名字>/build-<preset>/<config>/<名字>.dll
# 谁需要它在别处（例如测试工程要放在测试可执行文件旁边），谁自己拷 ——
# 产物永远属于插件，不因"被谁包含进来"而改变位置。
include("${NOSTOS_ROOT}/nostos/cmake/asan_runtime.cmake")

function(nostos_plugin_module name)
    add_library(${name} MODULE ${ARGN})

    # 只要 SDK 头与编译选项（/permissive- /EHsc、ASan 传播）；插件从不链接库的静态部分。
    target_link_libraries(${name} PRIVATE nostos::core)

    # 导出符号需要它（Windows 上 NOSTOS_EXPORT 才会展开成 dllexport）。
    target_compile_definitions(${name} PRIVATE NOSTOS_BUILDING_PLUGIN=1)

    # 不要 "lib" 前缀：宿主按名字在可执行文件旁查找模块。
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")

    nostos_plugin_artifact_dir(${name})

    if(MSVC)
        target_compile_options(${name} PRIVATE /permissive- /EHsc /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra)
    endif()
    nostos_copy_asan_runtime(${name})
endfunction()

# 把**产物**（模块 / 一体编译的单 exe）钉在本插件自己的目录里：
#     plugin/<名字>/build-<标签>/<config>/        （多配置生成器多一层 <config>，与 CMake 惯例一致）
# 不论谁构建它 —— 自己 build、还是被别的工程 add_subdirectory 进来 —— 路径都一样。
# （被包含时 CMake 的*二进制*目录会落在包含者的树里，那是临时对象；产物必须属于插件。）
#
# 标签来自 NOSTOS_PRESET_LABEL（各插件的 CMakePresets.json 里设好，与 preset 同名）；
# 没设时保持 CMake 默认（= 本工程构建树），这样把插件目录单独拷出去也能编。
function(nostos_plugin_artifact_dir target)
    if(NOT NOSTOS_PRESET_LABEL)
        return()
    endif()
    set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/build-${NOSTOS_PRESET_LABEL}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_dir}"
        LIBRARY_OUTPUT_DIRECTORY "${_dir}")
endfunction()
