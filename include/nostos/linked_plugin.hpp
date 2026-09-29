#pragma once
// nostos —— 静态链接进来的插件（与 loader.hpp 的"运行期加载"相对）。
//
// 同一个 plugin.cpp 可以编两次，源码一行不改：
//
//   ┌ 编成模块（定义 NOSTOS_BUILDING_PLUGIN）─→ 宿主用 DynamicPlugin 运行期 dlopen
//   └ 直接编进宿主可执行文件 ─────────────────→ 宿主用 LinkedPlugin 驱动
//
// 两侧共用 abi::PluginSession（abi/plugin_session.hpp），所以校验、激活会话、失败回滚、
// 依赖者先行、状态转移、逻辑卸载的语义**逐字相同**；差别只剩"有没有一个模块被映射进来"。
//
// 用法（宿主侧）：
//
//     NOSTOS_PLUGIN_ENTRY_DECL();                     // 声明链接进来的那个入口符号
//     auto plugin = nostos::LinkedPlugin::from(&nostos_plugin_entry);
//     plugin.activate(host.abi_bridge());             // 与动态模式完全一样的调用
//     ...
//     plugin.deactivate();
//
// 为什么还要保留 `nostos::abi::Host` 这层 C 包装（而不是直接给插件一个 C++ 引用）：
// 就是为了让**两种模式的插件代码完全相同** —— 插件那一侧永远只认识冻结的 host_api 表，
// 于是"能编成插件"的代码天然也能"编进宿主"，反之亦然。这条一致性本身就是这套 ABI 的测试。

#include "nostos/abi/host_api.hpp"        // HostBridge：会话需要完整类型
#include "nostos/abi/plugin_session.hpp"
#include "nostos/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nostos {

class LinkedPlugin {
public:
    // 与动态模式唯一不同的输入：不是模块路径，而是那个链接进来的入口函数地址。
    using EntryFn = const nostos_plugin_api* (*)();

    LinkedPlugin() noexcept = default;
    explicit LinkedPlugin(EntryFn entry) { attach(entry); }

    // 具名构造，读起来像一句声明；label 只用于诊断（默认 "linked plugin"）。
    static LinkedPlugin from(EntryFn entry, std::string_view label = "linked plugin") {
        LinkedPlugin plugin;
        plugin.attach(entry, label);
        return plugin;
    }

    // 取表并校验。label 只用于诊断（默认 "linked plugin"；宿主可以传自己的模块名）。
    void attach(EntryFn entry, std::string_view label = "linked plugin") {
        if (entry == nullptr) throw plugin_error("nostos: linked plugin has no entry function");
        const nostos_plugin_api* api = nullptr;
        try {
            api = entry();
        } catch (...) {
            // 与动态模式同一条纪律：异常不许穿过边界（这里连边界都是同一个进程，仍然照查）。
            throw plugin_error("nostos: plugin '" + std::string(label) +
                               "' let an exception escape nostos_plugin_entry(); the ABI forbids "
                               "exceptions across the boundary");
        }
        session_.attach(api, label);
    }

    const PluginInfo& info() const noexcept { return session_.info(); }
    bool loaded() const noexcept { return session_.attached(); }
    bool active() const noexcept { return session_.active(); }

    // 与 DynamicPlugin 同名同形：两种模式的宿主代码因此可以共用（见 app/main.cpp）。
    void activate(abi::HostBridge& host) { session_.activate(host); }
    void deactivate() noexcept { session_.deactivate(); }

    bool save_state(void* buf, std::size_t capacity, std::size_t* out_len) const noexcept {
        return session_.save_state(buf, capacity, out_len);
    }
    bool load_state(const void* buf, std::size_t len) noexcept {
        return session_.load_state(buf, len);
    }

    // 逻辑卸载。静态模式下没有模块要 unmap，但语义与 DynamicPlugin::unload 对齐，
    // 于是"卸载"这件事在两种模式里都是同一个调用。
    void unload() noexcept { session_.reset(); }

private:
    abi::PluginSession<abi::HostBridge> session_;
};

}  // namespace nostos
