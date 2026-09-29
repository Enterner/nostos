#pragma once
// 驱动一张插件表：校验 → 激活会话 → 回滚 → 状态转移。
//
// 这是**两种编译模式共用**的那一份逻辑：
//   · 动态加载：dlopen 找到 nostos_plugin_entry()（loader.cpp 的 DynamicPlugin）
//   · 静态链接：直接取链接进来的 nostos_plugin_entry 地址（linked_plugin.hpp 的 LinkedPlugin）
// 差别只剩"有没有一个模块被映射进来"；会话语义（发布归会话、失败不留残渣、依赖者先回滚、
// 默认逻辑卸载、逻辑卸载先于物理卸载）因此不可能在两种模式之间走岔。
//
// 为什么是模板（`PluginSession<Bridge>`）而不是直接吃 `abi::HostBridge&`：
// loader.hpp 刻意保持**最小编译面** —— 它只前向声明 HostBridge，不把 context/event 那一层
// 拖进每个"只想加载插件"的 TU。把 Bridge 做成模板参数，这个头就只依赖冻结的 C 契约、
// PluginInfo 与 error.hpp；而它的成员函数体内的 `Bridge::Guard`、`bridge_->core()` 都是
// 依赖名，只在实例化处（loader.cpp / linked_plugin.hpp 的使用方）才需要完整的 Bridge。

#include "nostos/abi/nostos_abi.h"
#include "nostos/abi/interface.hpp"     // status_name（诊断用）
#include "nostos/abi/plugin_info.hpp"
#include "nostos/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace nostos::abi {

template <class Bridge>
class PluginSession {
public:
    PluginSession() = default;
    PluginSession(const PluginSession&) = delete;
    PluginSession& operator=(const PluginSession&) = delete;

    // 可移动：宿主侧容器（DynamicPlugin/LinkedPlugin）要能按值返回与搬移。
    // 搬移只是转移"会话状态"，不动任何模块映射；被搬空的源变成未接管状态。
    PluginSession(PluginSession&& other) noexcept
        : api_(other.api_),
          bridge_(other.bridge_),
          state_(other.state_),
          active_(other.active_),
          info_(std::move(other.info_)) {
        other.api_ = nullptr;
        other.bridge_ = nullptr;
        other.state_ = nullptr;
        other.active_ = false;
        other.info_ = PluginInfo{};
    }

    PluginSession& operator=(PluginSession&& other) noexcept {
        if (this != &other) {
            reset();  // 目标是活跃的：先按纪律收尾，再接管源
            api_ = other.api_;
            bridge_ = other.bridge_;
            state_ = other.state_;
            active_ = other.active_;
            info_ = std::move(other.info_);
            other.api_ = nullptr;
            other.bridge_ = nullptr;
            other.state_ = nullptr;
            other.active_ = false;
            other.info_ = PluginInfo{};
        }
        return *this;
    }

    // 校验并接管一张插件表；source 只用于诊断（模块路径，或静态链接时的名字）。
    // 病态表被点名拒绝：struct_size 太老 / abi_version 太老 / 没名字 / 缺 activate 或 deactivate。
    void attach(const nostos_plugin_api* api, std::string_view source) {
        if (api == nullptr)
            throw plugin_error("nostos: plugin '" + std::string(source) +
                               "' returned a null plugin table");
        if (api->struct_size < NOSTOS_PLUGIN_API_V1_SIZE)
            throw plugin_error("nostos: plugin '" + std::string(source) + "' reports struct_size " +
                               std::to_string(api->struct_size) + " but this host needs at least " +
                               std::to_string(NOSTOS_PLUGIN_API_V1_SIZE) +
                               " — rebuild it against the current nostos/abi/nostos_abi.h");
        if (api->abi_version < NOSTOS_ABI_VERSION)
            throw plugin_error("nostos: plugin '" + std::string(source) + "' targets ABI version " +
                               std::to_string(api->abi_version) + " but this host implements " +
                               std::to_string(NOSTOS_ABI_VERSION));
        if (api->name == nullptr || api->name[0] == '\0')
            throw plugin_error("nostos: plugin '" + std::string(source) + "' has no name");
        if (api->activate == nullptr || api->deactivate == nullptr)
            throw plugin_error("nostos: plugin '" + std::string(api->name) +
                               "' does not implement activate/deactivate");

        api_ = api;
        info_.name = api->name;  // 表里的名字属于插件模块：要留存就拷贝（纪律 5）
        info_.version = api->version;
        info_.has_save_state = api->save_state != nullptr;
        info_.has_load_state = api->load_state != nullptr;
    }

    bool attached() const noexcept { return api_ != nullptr; }
    bool active() const noexcept { return active_; }
    void* state() const noexcept { return state_; }
    const PluginInfo& info() const noexcept { return info_; }

    // 原始插件表（宿主策略用：读 manifest 等尾部追加字段；NOSTOS_PLUGIN_HAS 门控）。
    // 未加载时为 NULL。
    const nostos_plugin_api* api() const noexcept { return api_; }

    void activate(Bridge& host) {
        if (api_ == nullptr) throw plugin_error("nostos: plugin is not loaded");
        if (active_) throw plugin_error("nostos: plugin '" + info_.name + "' is already active");

        // 从这里开始插件发布/订阅的一切都属于这个会话：无论它记不记得撤销、无论激活是否
        // 中途失败，会话结束即撤销（并且先做依赖者回滚）。
        host.begin_session();
        nostos_status status = NOSTOS_ERR_FAILED;
        try {
            const typename Bridge::Guard guard{host};
            status = api_->activate(host.api(), &state_);
        } catch (...) {
            // ABI 禁止异常跨界（纪律 2）：说清楚，而不是让外部异常穿过宿主栈。
            host.end_session();
            state_ = nullptr;
            throw plugin_error("nostos: plugin '" + info_.name +
                               "' let an exception escape activate(); the ABI forbids exceptions "
                               "across the boundary");
        }

        if (status != NOSTOS_OK) {
            host.end_session();
            state_ = nullptr;
            throw plugin_error("nostos: plugin '" + info_.name + "' failed to activate (" +
                               status_name(status) + "); per the ABI contract it left nothing "
                               "behind and will not be called again");
        }
        bridge_ = &host;
        active_ = true;
    }

    void deactivate() noexcept {
        if (!active_ || api_ == nullptr) return;
        // 依赖者先行：拿着插件发布的表的原生组件必须先松手。
        if (bridge_ != nullptr) bridge_->revoke_session_dependents();

        nostos_status status = NOSTOS_ERR_FAILED;
        try {
            const typename Bridge::Guard guard{*bridge_};
            status = api_->deactivate(state_);
        } catch (...) {
            status = NOSTOS_ERR_FAILED;  // noexcept：收尾不许终止
        }
        if (status != NOSTOS_OK && bridge_ != nullptr) {
            bridge_->core().log(4, "nostos: plugin '" + info_.name + "' reported " +
                                       status_name(status) + " while deactivating; the host "
                                       "revoked its publications anyway");
        }
        if (bridge_ != nullptr) bridge_->end_session();
        state_ = nullptr;
        active_ = false;
        bridge_ = nullptr;
    }

    bool save_state(void* buf, std::size_t capacity, std::size_t* out_len) const noexcept {
        if (!active_ || api_ == nullptr || api_->save_state == nullptr) return false;
        try {
            const typename Bridge::Guard guard{*bridge_};
            return api_->save_state(state_, buf, capacity, out_len) == NOSTOS_OK;
        } catch (...) {
            return false;
        }
    }

    bool load_state(const void* buf, std::size_t len) noexcept {
        if (!active_ || api_ == nullptr || api_->load_state == nullptr) return false;
        try {
            const typename Bridge::Guard guard{*bridge_};
            return api_->load_state(state_, buf, len) == NOSTOS_OK;
        } catch (...) {
            return false;
        }
    }

    // 逻辑卸载 + 忘掉这张表（不动任何模块映射：那是 DynamicLibrary 的事）。
    void reset() noexcept {
        deactivate();
        api_ = nullptr;
        info_ = PluginInfo{};
    }

private:
    const nostos_plugin_api* api_ = nullptr;  // 表本身由插件模块持有，这里只借指针
    Bridge* bridge_ = nullptr;
    void* state_ = nullptr;
    bool active_ = false;
    PluginInfo info_{};
};

}  // namespace nostos::abi
