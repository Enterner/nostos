# src/ —— 只有 loader 的实现，没有别的

这个目录故意只有一个文件：`loader/loader.cpp`。

框架的其余部分**全是头文件**（旁边的 `../include/nostos/`），不需要编译。平台相关的代码只有 loader
一处要落地成 .cpp，原因写在 `loader.cpp` 头部注释与 `docs/design.md` §6：

- 把 `<windows.h>` / `<dlfcn.h>` 关在单独一个 TU 里，包含 `nostos/loader.hpp` 的宿主 TU 就不会
  被平台头污染（完整 `windows.h` 的 `#define interface struct` 会打爆 `Context::interface`）；
- "核心零依赖 header-only" 的承诺因此继续成立 —— 不用 loader 的用户一行都不用编。

应用程序的源码不在这里：宿主应用在 `app/`，插件在 `plugin/`。
装给第三方的包只包含 `include/nostos/**` + 编译好的 `nostos::loader`（见 `nostos_test/package_consumer/`）。
