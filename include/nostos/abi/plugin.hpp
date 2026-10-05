#pragma once
// nostos L2 —— C ABI 边界的 plugin 侧（docs/design.md §4.7）。
//
// plugin 包含本头文件（加上它所用服务的共享接口头），定义一个类，然后以
// NOSTOS_PLUGIN 结束文件：
//
//     #include <nostos/abi/plugin.hpp>
//     #include <nostos/svc_id.hpp>   // 为 nostos::svc_id / nostos::event_id ——
//                                    // plugin.hpp 刻意不引入 C++ 核心，所以
//                                    // 本示例要按原文编译还需要这一个额外的
//                                    // include
//
//     struct MyPlugin {
//         static constexpr std::string_view name = "my.plugin";
//         static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(1, 0, 0);
//
//         nostos_status activate(const nostos::abi::Host& host) {
//             logger_ = host.service<LoggerTable>(nostos::svc_id<"demo.logger">);
//             if (logger_ == nullptr) return NOSTOS_ERR_FAILED;   // 点名报错，不做哑失败
//             ping_ = host.on<PingEvent>(nostos::event_id<PingEvent>, [this](const void* p) {
//                 logger_->log(logger_, "ping");
//             });
//             return NOSTOS_OK;
//         }
//
//         void deactivate() noexcept { ping_ = {}; }   // 每个 guard 都是 RAII
//     };
//
//     NOSTOS_PLUGIN(MyPlugin)
//
// 这里的每一样东西在构造上都是无异常的：plugin 自己的 C++ 异常在边界处被
// 捕获并报告为 NOSTOS_ERR_FAILED，而且每项宿主能力在使用前都按 struct_size
// 门控，因此 plugin 也能在更老的宿主上加载。

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "nostos/abi/nostos_abi.h"
#include "nostos/abi/interface.hpp"

// version 按 ABI 预期的格式打包：major<<24 | minor<<16 | patch
#define NOSTOS_PLUGIN_VERSION(major, minor, patch) \
    ((static_cast<std::uint32_t>(major) << 24) | (static_cast<std::uint32_t>(minor) << 16) | \
     static_cast<std::uint32_t>(patch))

// 唯一导出的符号。必须在文件作用域、恰好一个 TU 中使用。
#define NOSTOS_PLUGIN(PLUGIN_TYPE)                                                     \
    extern "C" NOSTOS_EXPORT const ::nostos_plugin_api* nostos_plugin_entry(void) {    \
        return ::nostos::abi::PluginEntry<PLUGIN_TYPE>::api();                         \
    }

// 另一侧的声明：当同一个 plugin.cpp 被**直接编进宿主**（静态链接模式，见 linked_plugin.hpp）
// 时，宿主用这一行声明那个入口符号，然后 LinkedPlugin::from(&nostos_plugin_entry) 驱动它。
// 与 NOSTOS_PLUGIN 成对存在，是为了让"两种编译模式"在源码上一眼看得出来。
#define NOSTOS_PLUGIN_ENTRY_DECL() extern "C" const ::nostos_plugin_api* nostos_plugin_entry(void)

namespace nostos::abi {

// plugin 眼中的宿主：把冻结的表包上一层，使缺失或过旧的能力成为编译期可知
// 的分支，而不是一次错误的调用。
class Host {
public:
    Host() noexcept = default;
    explicit Host(const nostos_host_api* api) noexcept : api_(api) {}

    // 是否根本可用：宿主填充了 v1 前缀，且其 ABI 版本不低于本 plugin 所需。
    bool valid() const noexcept {
        return api_ != nullptr && api_->struct_size >= NOSTOS_HOST_API_V1_SIZE &&
               api_->abi_version >= NOSTOS_ABI_VERSION;
    }

    const nostos_host_api* raw() const noexcept { return api_; }

    bool can_publish() const noexcept { return NOSTOS_HOST_HAS(api_, publish); }
    bool can_emit() const noexcept { return NOSTOS_HOST_HAS(api_, emit); }

    // ---- 服务 ---------------------------------------------------------------

    // 解析一张 published 表。表不存在*或*其修订无法满足本 plugin 时都返回
    // nullptr——若两者的差别要紧，借助宿主的日志查看。
    template <Table T>
    const T* service(std::uint64_t svc_id, std::uint32_t min_version = 1) const noexcept {
        if (!valid()) return nullptr;
        return query<T>(api_, svc_id, min_version);
    }

    const void* service(std::uint64_t svc_id) const noexcept {
        return valid() && api_->service != nullptr ? api_->service(svc_id) : nullptr;
    }

    // 发布本 plugin 自己的表。所有权留在 plugin 手中；即便 plugin 忘了，
    // 宿主也会在激活结束时撤销它；unpublish 只用于提前撤下。
    //
    // 生命周期规则（与宿主侧相同，见 abi/interface.hpp）：注册表绝不拥有表，
    // 也无法检查存储期，因此表至少要与激活同寿——作为 plugin 对象的成员，
    // 或一个 static。局部表在 activate() 返回的那一刻悬垂，会话记账也不会
    // 察觉。plugin 对象本身由 loader 恰好在会话期间持有，这正是成员表成为
    // 常规形态的原因。
    nostos_status publish(std::uint64_t svc_id, const void* table) const noexcept {
        if (!can_publish() || api_->publish == nullptr) return NOSTOS_ERR_BAD_STATE;
        return api_->publish(svc_id, table);
    }

    void unpublish(std::uint64_t svc_id) const noexcept {
        if (can_publish() && api_->unpublish != nullptr) api_->unpublish(svc_id);
    }

    // ---- 投递器 -------------------------------------------------------------

    // 取用绑定到本次激活会话的投递器（proposal-kit §6.1）。只在 activate()
    // 期间合法（此时本桥已安装）。没有该追加槽位的宿主报告
    // NOSTOS_ERR_BAD_STATE——与 publish() 对缺失能力用同一约定——于是调用方
    // 统一按“没有投递器”处理，不必手工探查 struct 大小。
    nostos_status acquire_poster(struct nostos_poster* out) const noexcept {
        if (!valid() || !NOSTOS_HOST_HAS(api_, acquire_poster) || api_->acquire_poster == nullptr)
            return NOSTOS_ERR_BAD_STATE;
        return api_->acquire_poster(out);
    }

    // ---- published 枚举（跨 ABI 域的“发现”）---------------------------------

    // 宿主实现了枚举槽位时为 true（尾部追加的槽位，旧宿主没有）。
    bool can_published_list() const noexcept {
        return valid() && NOSTOS_HOST_HAS(api_, published_list) && api_->published_list != nullptr;
    }

    // 枚举宿主当前 published 的服务：最多写 cap 个条目（注册序），返回写入数；
    // cap == 0 或 out == NULL 返回当前总数（不写入）。name 随注册表条目存活，
    // unpublish / 重新发布后失效——要留存必须拷贝。旧宿主返回 0（先问
    // can_published_list() 可区分“没有该能力”与“当前没有服务”）。
    std::uint32_t published_list(struct nostos_published_entry* out,
                                 std::uint32_t cap) const noexcept {
        if (!can_published_list()) return 0;
        return api_->published_list(out, cap);
    }

    // ---- 事件 ---------------------------------------------------------------

    // 拥有自己的可调用对象的订阅：销毁它即退订。
    class Subscription {
    public:
        Subscription() noexcept = default;
        Subscription(const nostos_host_api* api, std::uint32_t token,
                     std::shared_ptr<void> keep_alive) noexcept
            : api_(api), token_(token), keep_alive_(std::move(keep_alive)) {}

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept
            : api_(other.api_), token_(other.token_), keep_alive_(std::move(other.keep_alive_)) {
            other.api_ = nullptr;
            other.token_ = 0;
        }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                unsubscribe();
                api_ = other.api_;
                token_ = other.token_;
                keep_alive_ = std::move(other.keep_alive_);
                other.api_ = nullptr;
                other.token_ = 0;
            }
            return *this;
        }
        ~Subscription() { unsubscribe(); }

        void unsubscribe() noexcept {
            if (token_ != 0 && api_ != nullptr && api_->off != nullptr) api_->off(token_);
            token_ = 0;
            keep_alive_.reset();
        }

        std::uint32_t token() const noexcept { return token_; }
        explicit operator bool() const noexcept { return token_ != 0; }

    private:
        const nostos_host_api* api_ = nullptr;
        std::uint32_t token_ = 0;
        std::shared_ptr<void> keep_alive_;
    };

    // fn 收到的是事件的共享头 C struct，仅本次调用有效（纪律 5）——要留存的
    // 一律拷贝。
    template <typename F>
    Subscription on(std::uint64_t evt_id, F fn) const {
        if (!valid() || api_ == nullptr || api_->on == nullptr) return {};
        auto callable = std::make_shared<F>(std::move(fn));
        const std::uint32_t token = api_->on(evt_id, &trampoline<F>, callable.get());
        if (token == 0) return {};
        return Subscription{api_, token, std::move(callable)};
    }

    // 向宿主引发一个事件：原生监听者把该 struct 看作其 C++ 类型。在没有这些
    // 追加字段的宿主上不可用。
    void emit(std::uint64_t evt_id, const void* payload) const noexcept {
        if (can_emit() && api_->emit != nullptr) api_->emit(evt_id, payload);
    }

    // ---- 日志 ---------------------------------------------------------------

    void log(int level, const char* msg) const noexcept {
        if (valid() && api_->log != nullptr) api_->log(level, msg);
    }
    void log(int level, std::string_view msg) const noexcept {
        // 宿主只承诺调用期有效，所以交给它一份以 NUL 结尾的拷贝，让这条契约
        // 一目了然。
        const std::string copy{msg};
        log(level, copy.c_str());
    }

private:
    template <typename F>
    static void trampoline(void* user, const void* payload) noexcept {
        try {
            (*static_cast<F*>(user))(payload);
        } catch (...) {
            // 异常绝不跨越边界（纪律 2）。
        }
    }

    const nostos_host_api* api_ = nullptr;
};

// ---- plugin 入口点 -----------------------------------------------------------

namespace detail {

template <typename P>
concept NamedPlugin = requires {
    { P::name } -> std::convertible_to<std::string_view>;
};

template <typename P>
concept VersionedPlugin = requires {
    { P::version } -> std::convertible_to<std::uint32_t>;
};

// 把静态名字物化为以 NUL 结尾、plugin 生命周期的字符串。
template <typename P>
const char* plugin_name() {
    static const std::string owned{std::string_view{P::name}};
    return owned.c_str();
}

template <typename P>
concept HasSaveState = requires(P& p, void* buf, std::size_t cap, std::size_t* len) {
    { p.save_state(buf, cap, len) } -> std::convertible_to<nostos_status>;
};

template <typename P>
concept HasLoadState = requires(P& p, const void* buf, std::size_t len) {
    { p.load_state(buf, len) } -> std::convertible_to<nostos_status>;
};

template <typename P>
concept HasDeactivate = requires(P& p) { p.deactivate(); };

// 包装层从 noexcept 函数（PluginEntry::deactivate）里调用 deactivate()，
// 因此会抛异常的版本将不带任何诊断地 terminate。静态组件一侧对 onStop()
// 有镜像的检查（host_builder.hpp）；这让两条生命周期同样严格。
template <typename P>
concept DeactivateIsNoexcept = !HasDeactivate<P> || requires(P& p) {
    { p.deactivate() } noexcept;
};

template <typename P>
concept ActivateReturnsStatus = requires(P& p, const Host& host) {
    { p.activate(host) } -> std::convertible_to<nostos_status>;
};

template <typename P>
concept ActivateReturnsVoid = requires(P& p, const Host& host) { p.activate(host); };

}  // namespace detail

// 把 plugin 类变成冻结的 C 表。plugin 的状态由 plugin 分配、由 plugin
// 释放（纪律 1）。
template <typename P>
class PluginEntry {
public:
    static_assert(detail::NamedPlugin<P>,
                  "nostos: a plugin needs a name — static constexpr std::string_view name = ...");
    static_assert(detail::VersionedPlugin<P>,
                  "nostos: a plugin needs a version — static constexpr std::uint32_t version = "
                  "NOSTOS_PLUGIN_VERSION(major, minor, patch)");
    static_assert(detail::ActivateReturnsStatus<P> || detail::ActivateReturnsVoid<P>,
                  "nostos: a plugin needs nostos_status activate(const nostos::abi::Host&)");
    static_assert(detail::DeactivateIsNoexcept<P>,
                  "nostos: plugin deactivate() must be declared noexcept — rollback must not "
                  "fail, and the wrapper calls it from a noexcept function");

    static const nostos_plugin_api* api() noexcept {
        static const nostos_plugin_api table{
            static_cast<std::uint32_t>(sizeof(nostos_plugin_api)),
            NOSTOS_ABI_VERSION,
            detail::plugin_name<P>(),
            static_cast<std::uint32_t>(P::version),
            &activate,
            &deactivate,
            save_state_slot(),
            load_state_slot(),
            manifest_slot(),
        };
        return &table;
    }

private:
    // 未实现状态转移的 plugin 导出 NULL，而不是一个永远失败的占位桩：宿主
    // 因此能区分“不支持”与“试过但失败”（PluginInfo 的 has_save_state）。
    static constexpr auto save_state_slot() noexcept {
        if constexpr (detail::HasSaveState<P>) {
            return &PluginEntry::save_state;
        } else {
            return nullptr;
        }
    }

    static constexpr auto load_state_slot() noexcept {
        if constexpr (detail::HasLoadState<P>) {
            return &PluginEntry::load_state;
        } else {
            return nullptr;
        }
    }
    // manifest（可选）：插件类型若提供 `static const nostos_manifest* manifest`，
    // 则填入尾部追加的字段（宿主经 NOSTOS_PLUGIN_HAS 门控读取，用于懒激活、
    // 依赖记账与配置发现）。没有该成员的插件导出 NULL（完全可选）。
    static const nostos_manifest* manifest_slot() noexcept {
        if constexpr (requires { P::manifest; }) {
            return P::manifest;
        } else {
            return nullptr;
        }
    }

    static nostos_status activate(const nostos_host_api* host_api, void** out_state) noexcept {
        if (host_api == nullptr || out_state == nullptr) return NOSTOS_ERR_INVALID_ARG;
        const Host host{host_api};
        if (!host.valid()) return NOSTOS_ERR_BAD_ABI;
        try {
            std::unique_ptr<P> plugin = std::make_unique<P>();
            if constexpr (detail::ActivateReturnsStatus<P>) {
                const nostos_status status = plugin->activate(host);
                if (status != NOSTOS_OK) return status;  // unique_ptr 销毁造了一半的 plugin
            } else {
                plugin->activate(host);
            }
            *out_state = plugin.release();
            return NOSTOS_OK;
        } catch (...) {
            return NOSTOS_ERR_FAILED;
        }
    }

    static nostos_status deactivate(void* state) noexcept {
        if (state == nullptr) return NOSTOS_OK;  // 无状态 plugin 是合法的
        try {
            const std::unique_ptr<P> plugin{static_cast<P*>(state)};
            if constexpr (detail::HasDeactivate<P>) plugin->deactivate();
            return NOSTOS_OK;
        } catch (...) {
            return NOSTOS_ERR_FAILED;
        }
    }

    static nostos_status save_state(void* state, void* buf, std::size_t cap,
                                    std::size_t* out_len) noexcept {
        if (state == nullptr) return NOSTOS_ERR_BAD_STATE;
        try {
            return static_cast<P*>(state)->save_state(buf, cap, out_len);
        } catch (...) {
            return NOSTOS_ERR_FAILED;
        }
    }

    static nostos_status load_state(void* state, const void* buf, std::size_t len) noexcept {
        if (state == nullptr) return NOSTOS_ERR_BAD_STATE;
        try {
            return static_cast<P*>(state)->load_state(buf, len);
        } catch (...) {
            return NOSTOS_ERR_FAILED;
        }
    }
};

}  // namespace nostos::abi
