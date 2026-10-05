#pragma once
// nostos L2 —— 动态 plugin 加载（docs/design.md §4.7, §7 Phase 3）。
//
// 一个 plugin 是一个只导出一个符号的共享库：nostos_plugin_entry()，它返回一张
// 冻结的 C 函数表（nostos_abi.h）。plugin 自己的 C++ 设施——allocator、RTTI、
// std::string——一概不穿越边界，这正是 "MinGW plugin ↔ MSVC host" 的场景能够
// 成立的根本原因。
//
// 本头文件是面向 host 的一半，刻意不包含任何平台头：<windows.h> 会把它的宏泄漏
// 进每个加载 plugin 的翻译单元。平台代码住在 src/loader/loader.cpp，header-only
// 的核心保持 header-only。
//
// 卸载语义（docs/design.md §4.6）：deactivate() 是*逻辑*卸载——plugin 拆掉自己的
// 世界，host 撤销 plugin 发布过的东西，代码保持映射。unmap 是显式动作（unload()
// 或析构函数）：只要 plugin 交出去的任何东西、或它启动的任何线程仍然可达，
// plugin 就无法被安全地 unmap。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nostos/abi/nostos_abi.h"
#include "nostos/abi/plugin_info.hpp"
#include "nostos/abi/plugin_session.hpp"
#include "nostos/error.hpp"
#include "nostos/svc_id.hpp"

namespace nostos {

namespace abi {
class HostBridge;
}

// 正在运行的可执行文件所在目录——plugin 模块通常就放在这里。
std::string executable_directory();

// 可执行文件旁边的 plugin 模块路径：`name` 未带平台模块扩展名时自动补上。
std::string plugin_module_path(std::string_view name);

// 把按 (major<<24|minor<<16|patch) 打包的版本号解成 "major.minor.patch"。
// 实现在 abi/plugin_info.hpp（inline）—— 静态链接插件也要用它，而那时不该有 loader。
inline std::string unpack_version(std::uint32_t packed) { return abi::unpack_version(packed); }

// 共享库的 RAII 句柄。
class DynamicLibrary {
public:
    DynamicLibrary() noexcept = default;
    ~DynamicLibrary();

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    // 模块无法加载（缺文件、架构不符、缺依赖）时抛出携带 OS 错误文本的
    // plugin_error。
    static DynamicLibrary open(const std::string& path);

    bool is_open() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return is_open(); }

    // 模块未导出该符号时为 nullptr。
    void* symbol(const char* name) const noexcept;

    const std::string& path() const noexcept { return path_; }

    // 物理 unmap。尽力而为且幂等；调用方保证本模块已无任何代码或数据可达。
    void close() noexcept;

private:
    void* handle_ = nullptr;
    std::string path_;
};

// PluginInfo 定义在 abi/plugin_info.hpp（动态加载与静态链接两种模式共用同一份）；
// nostos::PluginInfo 这个名字保持不变（见那个头里的 using）。

// 已加载的 plugin 模块。Move-only：生命周期恰好由一个所有者驱动。
class DynamicPlugin {
public:
    DynamicPlugin() noexcept = default;
    ~DynamicPlugin();

    DynamicPlugin(DynamicPlugin&& other) noexcept;
    DynamicPlugin& operator=(DynamicPlugin&& other) noexcept;
    DynamicPlugin(const DynamicPlugin&) = delete;
    DynamicPlugin& operator=(const DynamicPlugin&) = delete;

    // 加载模块并校验导出的表（struct_size、abi_version、必填字段）。抛出的
    // plugin_error 会同时给出路径与问题所在。
    static DynamicPlugin load(const std::string& path);

    const PluginInfo& info() const noexcept { return session_.info(); }
    const DynamicLibrary& library() const noexcept { return library_; }
    bool loaded() const noexcept { return session_.attached(); }
    bool active() const noexcept { return session_.active(); }

    // 原始插件表（宿主策略用：读 manifest 等尾部追加字段；NOSTOS_PLUGIN_HAS 门控）。
    // 未加载时为 NULL。
    const nostos_plugin_api* api() const noexcept { return session_.api(); }

    // 把 host 表交给 plugin，让它自行接线。plugin 报告失败时抛 plugin_error；
    // 失败的激活不留残余（按 ABI 契约，它发布的任何东西都不会存留）。
    void activate(abi::HostBridge& host);

    // 逻辑卸载：回滚 host 中依赖该 plugin 发布物的组件，要求 plugin 释放一切，
    // 并忘掉它的发布记录。noexcept 且幂等——teardown 不允许失败。
    void deactivate() noexcept;

    // 可选的状态转移（热重载，Phase 4）。plugin 不支持时返回 false。绝不跨边界
    // 序列化：缓冲区归 host 所有，做的是拷贝。
    bool save_state(void* buf, std::size_t capacity, std::size_t* out_len) const noexcept;
    bool load_state(const void* buf, std::size_t len) noexcept;

    // 物理 unmap：先 deactivate()，再关闭库。noexcept；deactivate 从未成功过时
    // 模块保持映射。
    void unload() noexcept;

private:
    DynamicLibrary library_;
    // 校验 + 激活会话 + 回滚 + 状态转移都在这里，与"表从哪来"无关；
    // 静态链接模式用的是同一个类（abi::PluginSession，linked_plugin.hpp）。
    abi::PluginSession<abi::HostBridge> session_;
};

// 懒激活策略（宿主可选装配，docs/proposal-kit.md §6.3）：
// manifest 声明了 inject 的模块，在依赖服务就绪前不 activate（登记 PENDING）；
// published 注册表每次变化后调用 pump_into() 重估，依赖齐了自动拉起。
// 已激活插件的依赖被撤销时，回滚由既有语义覆盖（revoke_dependents 依赖者先行）——
// 本类只负责"还没激活的那一半"。
class LazyGate {
public:
    explicit LazyGate(abi::HostBridge& bridge) noexcept : bridge_(&bridge) {}

    // 尝试激活（宿主持有返回 true 后的插件；未就绪的由 gate 持有）。
    //   依赖齐（或无 manifest）⇒ 立即 activate 并返回 true；
    //   依赖缺失 ⇒ 登记 PENDING 并返回 false（宿主稍后 pump）。
    // activate 的异常原样传播（与直接调用 DynamicPlugin::activate 一致）。
    bool submit(DynamicPlugin& plugin);

    // published 注册表变化后重估 PENDING：激活所有依赖已就绪者，
    // 并把它们作为返回值交还给调用者（调用者负责最终的逆序卸载）。
    std::vector<DynamicPlugin> pump_into();

    std::size_t pending_count() const noexcept { return pending_.size(); }

private:
    struct Pending {
        std::vector<std::uint64_t> missing;   // 尚未出现的服务 id
        DynamicPlugin plugin;
    };
    abi::HostBridge* bridge_ = nullptr;
    std::vector<Pending> pending_;
};

}  // namespace nostos
