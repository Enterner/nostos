# ── AddressSanitizer 运行时助手 ────────────────────────────────────────────────
#
# MSVC 的 ASan 运行时是**动态**链接的，DLL 放在工具链目录里，而不是可执行文件旁：
# 刚构建出来的 nostos_tests.exe 会在 main() 之前就以 STATUS_DLL_NOT_FOUND 死掉 ——
# 这很容易被误判成"消毒器在这个环境里不能用"。
#
# 谁需要它：任何链接 nostos::core 的**可执行文件/模块**。库自己定义 NOSTOS_ASAN
# （那是库的要求），这里只负责把运行时拷到每个目标旁边。
#
# 用 include() 引入，而不是定义在库的 CMakeLists 里：CMake 的函数只对"定义它的目录及其
# 子目录"可见，而需要它的 plugin/ 与 nostos_test/ 都是库的**父**目录。

function(nostos_copy_asan_runtime target)
    if(NOT NOSTOS_ASAN OR NOT MSVC)
        return()
    endif()
    get_filename_component(cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB asan_runtimes "${cl_dir}/clang_rt.asan*.dll")
    if(NOT asan_runtimes)
        message(WARNING
            "nostos: AddressSanitizer is on but no clang_rt.asan*.dll was found next to "
            "${CMAKE_CXX_COMPILER}; put the MSVC bin directory on PATH before running the targets")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${asan_runtimes} "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying the AddressSanitizer runtime next to ${target}")
endfunction()
