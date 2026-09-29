#pragma once
// nostos L2 — dynamic plugin loading (docs/design.md §4.7, §7 Phase 3).
//
// A plugin is a shared library exporting one symbol, nostos_plugin_entry(),
// which returns a frozen C function table (nostos_abi.h). Nothing about the
// plugin's own C++ — its allocator, its RTTI, its std::string — crosses the
// boundary, which is what makes the "MinGW plugin ↔ MSVC host" case work at
// all.
//
// This header is the host-facing half and deliberately includes no platform
// header: <windows.h> would leak its macros into every translation unit that
// loads a plugin. The platform code lives in src/loader/loader.cpp, and the
// header-only core stays header-only.
//
// Unload semantics (docs/design.md §4.6): deactivate() is the *logical* unload
// — the plugin tears its world down, the host revokes what the plugin
// published, and the code stays mapped. Unmapping is explicit (unload() or the
// destructor) because a plugin cannot be safely unmapped while anything it
// handed out, or any thread it started, is still reachable.

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

// Directory of the running executable — where plugin modules normally sit.
std::string executable_directory();

// Path of a plugin module next to the executable: `name` gets the platform's
// module extension appended unless it already has one.
std::string plugin_module_path(std::string_view name);

// "major.minor.patch" for a version packed as (major<<24|minor<<16|patch)。
// 实现在 abi/plugin_info.hpp（inline）—— 静态链接插件也要用它，而那时不该有 loader。
inline std::string unpack_version(std::uint32_t packed) { return abi::unpack_version(packed); }

// RAII handle to a shared library.
class DynamicLibrary {
public:
    DynamicLibrary() noexcept = default;
    ~DynamicLibrary();

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    // Throws plugin_error carrying the OS error text when the module cannot be
    // loaded (missing file, wrong architecture, missing dependency).
    static DynamicLibrary open(const std::string& path);

    bool is_open() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return is_open(); }

    // nullptr when the module does not export the symbol.
    void* symbol(const char* name) const noexcept;

    const std::string& path() const noexcept { return path_; }

    // Physical unmap. Best-effort and idempotent; the caller guarantees that
    // no code or data of this module is reachable any more.
    void close() noexcept;

private:
    void* handle_ = nullptr;
    std::string path_;
};

// PluginInfo 现在住在 abi/plugin_info.hpp（动态加载与静态链接两种模式共用同一份）；
// nostos::PluginInfo 这个名字保持不变（见那个头里的 using）。

// A loaded plugin module. Move-only: exactly one owner drives the lifecycle.
class DynamicPlugin {
public:
    DynamicPlugin() noexcept = default;
    ~DynamicPlugin();

    DynamicPlugin(DynamicPlugin&& other) noexcept;
    DynamicPlugin& operator=(DynamicPlugin&& other) noexcept;
    DynamicPlugin(const DynamicPlugin&) = delete;
    DynamicPlugin& operator=(const DynamicPlugin&) = delete;

    // Loads the module and validates the exported table (struct_size,
    // abi_version, required fields). Throws plugin_error naming both the path
    // and what was wrong with it.
    static DynamicPlugin load(const std::string& path);

    const PluginInfo& info() const noexcept { return session_.info(); }
    const DynamicLibrary& library() const noexcept { return library_; }
    bool loaded() const noexcept { return session_.attached(); }
    bool active() const noexcept { return session_.active(); }

    // 原始插件表（宿主策略用：读 manifest 等尾部追加字段；NOSTOS_PLUGIN_HAS 门控）。
    // 未加载时为 NULL。
    const nostos_plugin_api* api() const noexcept { return session_.api(); }

    // Hand the plugin the host table and let it wire itself up. Throws
    // plugin_error when the plugin reports failure; a failed activation leaves
    // no residue (nothing it published survives, per the ABI contract).
    void activate(abi::HostBridge& host);

    // Logical unload: roll back the host components that depend on what the
    // plugin published, tell the plugin to release everything, and forget its
    // publications. noexcept and idempotent — teardown must not fail.
    void deactivate() noexcept;

    // Optional state transfer (hot reload, Phase 4). Returns false when the
    // plugin does not support it. Never serialises across the boundary: the
    // buffer is host-owned and copied.
    bool save_state(void* buf, std::size_t capacity, std::size_t* out_len) const noexcept;
    bool load_state(const void* buf, std::size_t len) noexcept;

    // Physical unmap: deactivate() first, then close the library. noexcept;
    // the module stays mapped if it was never deactivated successfully.
    void unload() noexcept;

private:
    DynamicLibrary library_;
    // 校验 + 激活会话 + 回滚 + 状态转移都在这里，与"表从哪来"无关；
    // 静态链接模式用的是同一个类（abi::PluginSession，linked_plugin.hpp）。
    abi::PluginSession<abi::HostBridge> session_;
};

// 懒激活策略（宿主可选装配，docs/proposal-kit.md §6.3）：
// manifest 声明了 inject 的模块，在依赖服务就绪前不 activate（登记 PENDING）；
// published 注册表每次变化后调用 pump() 重估，依赖齐了自动拉起。
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
