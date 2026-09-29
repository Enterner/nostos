#pragma once
// nostos L2 — the plugin side of the C ABI boundary (docs/design.md §4.7).
//
// A plugin includes this header (plus the shared interface headers of the
// services it uses), defines one class, and ends the file with NOSTOS_PLUGIN:
//
//     #include <nostos/abi/plugin.hpp>
//     #include <nostos/svc_id.hpp>   // for nostos::svc_id / nostos::event_id —
//                                    // plugin.hpp deliberately does not pull in
//                                    // the C++ core, so the example needs this
//                                    // one extra include to compile as written
//
//     struct MyPlugin {
//         static constexpr std::string_view name = "my.plugin";
//         static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(1, 0, 0);
//
//         nostos_status activate(const nostos::abi::Host& host) {
//             logger_ = host.service<LoggerTable>(nostos::svc_id<"demo.logger">);
//             if (logger_ == nullptr) return NOSTOS_ERR_FAILED;   // named, not silent
//             ping_ = host.on<PingEvent>(nostos::event_id<PingEvent>, [this](const void* p) {
//                 logger_->log(logger_, "ping");
//             });
//             return NOSTOS_OK;
//         }
//
//         void deactivate() noexcept { ping_ = {}; }   // every guard is RAII
//     };
//
//     NOSTOS_PLUGIN(MyPlugin)
//
// Everything here is exception-free by construction: a plugin's own C++
// exceptions are caught at the boundary and reported as NOSTOS_ERR_FAILED, and
// every host capability is gated on struct_size before it is used, so a plugin
// also loads against an older host.

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

// version packed as the ABI expects: major<<24 | minor<<16 | patch
#define NOSTOS_PLUGIN_VERSION(major, minor, patch) \
    ((static_cast<std::uint32_t>(major) << 24) | (static_cast<std::uint32_t>(minor) << 16) | \
     static_cast<std::uint32_t>(patch))

// The one exported symbol. Must be used at file scope, in exactly one TU.
#define NOSTOS_PLUGIN(PLUGIN_TYPE)                                                     \
    extern "C" NOSTOS_EXPORT const ::nostos_plugin_api* nostos_plugin_entry(void) {    \
        return ::nostos::abi::PluginEntry<PLUGIN_TYPE>::api();                         \
    }

// 另一侧的声明：当同一个 plugin.cpp 被**直接编进宿主**（静态链接模式，见 linked_plugin.hpp）
// 时，宿主用这一行声明那个入口符号，然后 LinkedPlugin::from(&nostos_plugin_entry) 驱动它。
// 与 NOSTOS_PLUGIN 成对存在，是为了让"两种编译模式"在源码上一眼看得出来。
#define NOSTOS_PLUGIN_ENTRY_DECL() extern "C" const ::nostos_plugin_api* nostos_plugin_entry(void)

namespace nostos::abi {

// What a plugin sees of the host: the frozen table, wrapped so that a missing
// or older capability is a compile-time-known branch rather than a bad call.
class Host {
public:
    Host() noexcept = default;
    explicit Host(const nostos_host_api* api) noexcept : api_(api) {}

    // Usable at all: the host filled the v1 prefix and speaks at least this
    // plugin's ABI version.
    bool valid() const noexcept {
        return api_ != nullptr && api_->struct_size >= NOSTOS_HOST_API_V1_SIZE &&
               api_->abi_version >= NOSTOS_ABI_VERSION;
    }

    const nostos_host_api* raw() const noexcept { return api_; }

    bool can_publish() const noexcept { return NOSTOS_HOST_HAS(api_, publish); }
    bool can_emit() const noexcept { return NOSTOS_HOST_HAS(api_, emit); }

    // ---- services ---------------------------------------------------------

    // Resolve a published table. Returns nullptr when it is absent *or* when
    // its revision cannot satisfy this plugin — check with the host's log if
    // the difference matters.
    template <Table T>
    const T* service(std::uint64_t svc_id, std::uint32_t min_version = 1) const noexcept {
        if (!valid()) return nullptr;
        return query<T>(api_, svc_id, min_version);
    }

    const void* service(std::uint64_t svc_id) const noexcept {
        return valid() && api_->service != nullptr ? api_->service(svc_id) : nullptr;
    }

    // Publish this plugin's own table. Ownership stays with the plugin, and
    // the host revokes it at the end of the activation even if the plugin
    // forgets; unpublish only to withdraw it earlier.
    //
    // Lifetime rule (same as the host side, see abi/interface.hpp): the registry
    // never owns the table and cannot check storage duration, so the table must
    // live at least as long as the activation — a member of the plugin object, or
    // a static. A local table here dangles the moment activate() returns, and the
    // session bookkeeping will not notice. The plugin object itself is owned by
    // the loader for exactly the length of the session, which is why a member
    // table is the normal shape.
    nostos_status publish(std::uint64_t svc_id, const void* table) const noexcept {
        if (!can_publish() || api_->publish == nullptr) return NOSTOS_ERR_BAD_STATE;
        return api_->publish(svc_id, table);
    }

    void unpublish(std::uint64_t svc_id) const noexcept {
        if (can_publish() && api_->unpublish != nullptr) api_->unpublish(svc_id);
    }

    // ---- events -----------------------------------------------------------

    // A subscription that owns its callable: dropping it unsubscribes.
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

    // fn takes the shared-header C struct of the event, valid for this call
    // only (discipline 5) — copy anything you keep.
    template <typename F>
    Subscription on(std::uint64_t evt_id, F fn) const {
        if (!valid() || api_ == nullptr || api_->on == nullptr) return {};
        auto callable = std::make_shared<F>(std::move(fn));
        const std::uint32_t token = api_->on(evt_id, &trampoline<F>, callable.get());
        if (token == 0) return {};
        return Subscription{api_, token, std::move(callable)};
    }

    // Raise an event into the host: native listeners see the struct as its
    // C++ type. Unavailable on a host that predates the appended fields.
    void emit(std::uint64_t evt_id, const void* payload) const noexcept {
        if (can_emit() && api_->emit != nullptr) api_->emit(evt_id, payload);
    }

    // ---- logging ----------------------------------------------------------

    void log(int level, const char* msg) const noexcept {
        if (valid() && api_->log != nullptr) api_->log(level, msg);
    }
    void log(int level, std::string_view msg) const noexcept {
        // The host only promises validity for the duration of the call, so
        // hand it a NUL-terminated copy and keep that contract obvious.
        const std::string copy{msg};
        log(level, copy.c_str());
    }

private:
    template <typename F>
    static void trampoline(void* user, const void* payload) noexcept {
        try {
            (*static_cast<F*>(user))(payload);
        } catch (...) {
            // Exceptions never cross the boundary (discipline 2).
        }
    }

    const nostos_host_api* api_ = nullptr;
};

// ---- the plugin entry point ------------------------------------------------

namespace detail {

template <typename P>
concept NamedPlugin = requires {
    { P::name } -> std::convertible_to<std::string_view>;
};

template <typename P>
concept VersionedPlugin = requires {
    { P::version } -> std::convertible_to<std::uint32_t>;
};

// Materialise the static name as a NUL-terminated string with plugin lifetime.
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

// The wrapper calls deactivate() from a noexcept function (PluginEntry::
// deactivate), so a throwing one would terminate with no diagnostic at all.
// The static-component side has the mirror-image check on onStop()
// (host_builder.hpp); this keeps the two lifecycles equally strict.
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

// Turns a plugin class into the frozen C table. The plugin's state is
// allocated by the plugin and freed by the plugin (discipline 1).
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
    // A plugin that does not implement state transfer exports NULL rather than
    // a stomp that always fails: the host can then tell "unsupported" from
    // "tried and failed" (has_save_state in PluginInfo).
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
                if (status != NOSTOS_OK) return status;  // unique_ptr destroys the half-built plugin
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
        if (state == nullptr) return NOSTOS_OK;  // a stateless plugin is legal
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
