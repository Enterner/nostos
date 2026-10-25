# src/ —— 平台相关代码的落地点，只有两个文件

    loader/loader.cpp   动态加载的平台部分（LoadLibraryExW / dlopen）
    kit/http.cpp        HTTP 服务器（winsock / posix socket）

框架的其余部分**全是头文件**（`../include/nostos/`）。平台头只在这两个 TU 里出现：

- 把 `<windows.h>` / `<winsock2.h>` / `<dlfcn.h>` 关在单独的 TU 里，包含对应公共头
  （`nostos/loader.hpp` / `nostos/kit/http.hpp`）的宿主/插件 TU 就不会被平台头污染
  （完整 `windows.h` 的 `#define interface struct` 会打爆 `Context::interface`）；
- "核心零依赖 header-only" 的承诺因此继续成立——不用 loader / kit_http 的用户
  一行都不用编。

应用程序的源码不在这里：本仓库只是库；宿主与插件是使用本库的独立工程。
装给第三方的包只包含 `include/nostos/**` + 编译好的 `nostos::loader` / `nostos::kit_http`。
