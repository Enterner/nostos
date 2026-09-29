#pragma once
// nostos L2 — the host side of the C ABI boundary (docs/design.md §4.7).
//
// HostBridge turns a HostCore into the stateless nostos_host_api table a
// plugin receives. Every thunk is noexcept and swallows exceptions: nothing
// may cross the boundary (discipline 2), including a host-side failure.
//
// The frozen table has no user pointer, so the thunks resolve "which host?"
// through a thread-local *current bridge*. Host<...> installs its bridge when
// it is created (install()), which means plugin code can call back into the
// host from anywhere on that thread — during activate(), from inside a
// published table function the host called, from an event callback — without
// every native caller having to remember a guard. Guard is the explicit,
// scoped form (the loader uses it around every call into plugin code) and the
// way to nest two bridges deliberately.
//
// Consequence, by design: one host per thread owns the ABI. A plugin calling
// from a thread the host never called it on gets NULL/0 rather than a wrong
// host, and two hosts constructed on one thread resolve to the most recently
// created one (the core is single-threaded, docs/design.md §4.8).
//
// Publish sessions: everything a plugin publishes between begin_session() and
// end_session() is remembered, so a table owned by a plugin can never outlive
// the plugin's activation — not on a failed activate, not if the plugin
// forgets to unpublish. Before those tables go away, the *dependents* are
// rolled back through the revoker hook Host<...> installs.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nostos/abi/nostos_abi.h"
#include "nostos/context.hpp"
#include "nostos/error.hpp"

namespace nostos::abi {

class HostBridge {
public:
    explicit HostBridge(HostCore& core) noexcept : core_(&core) {}

    HostBridge(const HostBridge&) = delete;
    HostBridge& operator=(const HostBridge&) = delete;

    // A bridge that was never installed resolves nothing — the null case the
    // thunks report as "no host".
    ~HostBridge() {
        if (current() == this) set_current(previous_);
    }

    HostCore& core() noexcept { return *core_; }

    // The table to hand to a plugin. Its address is stable for the process and
    // every field — including those appended after the v1 freeze — is filled.
    static const nostos_host_api* api() noexcept { return host_api_table(); }

    // ---- current bridge ---------------------------------------------------

    static HostBridge* current() noexcept { return current_slot(); }
    static void set_current(HostBridge* bridge) noexcept { current_slot() = bridge; }

    // Make this bridge the one the thunks resolve on this thread, until it is
    // destroyed or another bridge is installed. Idempotent. Called by
    // Host<...> when its bridge is first needed.
    void install() noexcept {
        if (current() == this) return;
        previous_ = current();
        set_current(this);
    }

    // RAII install/restore, allocation-free so it is safe inside a noexcept
    // thunk. A plugin callback re-enters through its own Guard.
    struct [[nodiscard]] Guard {
        explicit Guard(HostBridge& bridge) noexcept
            : bridge_(&bridge), previous_(HostBridge::current()) {
            HostBridge::set_current(&bridge);
        }
        ~Guard() {
            if (bridge_ != nullptr) HostBridge::set_current(previous_);
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept : bridge_(other.bridge_), previous_(other.previous_) {
            other.bridge_ = nullptr;
        }

    private:
        HostBridge* bridge_;
        HostBridge* previous_;
    };

    // ---- publish / subscribe sessions -------------------------------------

    // What the session has added so far. Both halves are tracked for the same
    // reason: the host must be able to take back what the plugin handed it.
    struct SessionMark {
        std::size_t published = 0;
        std::size_t subscriptions = 0;
        std::size_t posters = 0;
    };

    void begin_session() {
        session_marks_.push_back(
            SessionMark{published_.size(), subscriptions_.size(), session_posters_.size()});
    }

    // Roll back native components that depend on the services published during
    // the current session — dependents first, before the plugin itself is
    // deactivated. Returns how many services had dependents rolled back.
    std::size_t revoke_session_dependents() {
        std::size_t count = 0;
        const std::size_t mark = session_marks_.empty() ? 0 : session_marks_.back().published;
        for (std::size_t i = published_.size(); i > mark; --i)
            if (revoke_dependents(published_[i - 1])) ++count;
        return count;
    }

    // Forget (and unpublish) everything published in the current session, and
    // unsubscribe everything the plugin subscribed during it.
    // noexcept: it runs on teardown paths, including a failed activation.
    void end_session() noexcept {
        if (session_marks_.empty()) return;
        const SessionMark mark = session_marks_.back();
        session_marks_.pop_back();
        for (std::size_t i = published_.size(); i > mark.published; --i)
            core_->published().remove(published_[i - 1]);
        published_.resize(mark.published);

        // The subscription half. A plugin that hands the host a callback and
        // never takes it back would leave the host dispatching into a module it
        // is about to unmap — and unlike a published table (which the host
        // resolves on demand), a subscription is a live pointer into plugin code.
        // When the outermost session ends the plugin is going away entirely, so
        // anything it subscribed outside a session goes too.
        const std::size_t from = session_marks_.empty() ? 0 : mark.subscriptions;
        for (std::size_t i = subscriptions_.size(); i > from; --i)
            core_->events().off_raw(subscriptions_[i - 1]);
        subscriptions_.resize(from);

        // The poster half. A poster acquired in this session stops accepting
        // work: a plugin that kept its own thread posting after deactivate
        // would have the host running plugin code past its teardown.
        for (std::size_t i = session_posters_.size(); i > mark.posters; --i)
            session_posters_[i - 1]->live = false;
        session_posters_.resize(mark.posters);
    }

    std::size_t session_depth() const noexcept { return session_marks_.size(); }
    bool in_session() const noexcept { return !session_marks_.empty(); }
    std::size_t published_in_session() const noexcept {
        return published_.size() - (session_marks_.empty() ? 0 : session_marks_.back().published);
    }
    std::size_t subscriptions_in_session() const noexcept {
        return subscriptions_.size() -
               (session_marks_.empty() ? 0 : session_marks_.back().subscriptions);
    }
    // Live subscriptions this bridge has handed out (session or not).
    std::size_t subscriptions_tracked() const noexcept { return subscriptions_.size(); }

    // ---- poster：跨线程投递（绑定桥；M1，docs/proposal-kit.md §6.1）--------

    // poster 的宿主侧状态。表指向它（桥所有 ⇒ 表随桥存活）；
    // pending 是线程安全的待执行队列，宿主主循环 drain_posted() 排水。
    struct PosterState {
        HostBridge* bridge = nullptr;
        std::mutex mu;
        std::vector<std::pair<void (*)(void*), void*>> pending;
        bool live = true;
    };

    // 表槽位（static：表构造器在类外引用）。自带锁；会话作废后返回 BAD_STATE。
    static nostos_status post_thunk(void* self, void (*fn)(void*), void* arg) noexcept {
        auto* ps = static_cast<PosterState*>(self);
        if (ps == nullptr || fn == nullptr) return NOSTOS_ERR_INVALID_ARG;
        std::lock_guard<std::mutex> lock(ps->mu);
        if (!ps->live) return NOSTOS_ERR_BAD_STATE;
        ps->pending.push_back({fn, arg});
        return NOSTOS_OK;
    }

    // 表槽位（static）：解析 TLS 上的当前桥并交给你 acquire_poster。
    // 只在 activate 期间（桥在 TLS 上）才会成功。
    static nostos_status acquire_poster_thunk(struct nostos_poster* out) noexcept {
        HostBridge* bridge = HostBridge::current();
        if (bridge == nullptr) return NOSTOS_ERR_BAD_STATE;
        try {
            return bridge->acquire_poster(out);
        } catch (...) {
            return NOSTOS_ERR_FAILED;
        }
    }

    // activate 期间领取（此时本桥在 TLS 上）。同一会话内幂等。
    nostos_status acquire_poster(struct nostos_poster* out) {
        if (out == nullptr) return NOSTOS_ERR_INVALID_ARG;
        posters_.emplace_back();
        PosterState& ps = posters_.back();
        ps.bridge = this;
        session_posters_.push_back(&ps);
        out->struct_size = sizeof(nostos_poster);
        out->self = &ps;
        out->post = &post_thunk;
        return NOSTOS_OK;
    }

    // 宿主主循环每拍调用：在**调用者线程**上以本桥的 Guard 执行全部已投递
    // 任务——任务体内的宿主 API 因此全部可用。fn 抛异常按 ABI 纪律兜底为日志。
    void drain_posted() {
        for (PosterState& ps : posters_) {
            std::vector<std::pair<void (*)(void*), void*>> batch;
            {
                std::lock_guard<std::mutex> lock(ps.mu);
                batch.swap(ps.pending);
            }
            if (!ps.live || batch.empty()) continue;
            for (auto& task : batch) {
                Guard guard(*this);
                try {
                    task.first(task.second);
                } catch (...) {
                    core_->log(4, "nostos: a posted task threw an exception; swallowed");
                }
            }
        }
    }

    // ---- dependent rollback hook ------------------------------------------

    // Host<...> installs this: only it knows its components, so only it can
    // roll back the ones that acquired a published table. Returns true when
    // something was rolled back.
    using DependentRevoker = std::function<bool(std::uint64_t)>;

    void set_dependent_revoker(DependentRevoker revoker) {
        revoker_ = std::move(revoker);
        warned_no_revoker_ = false;
    }
    bool has_dependent_revoker() const noexcept { return static_cast<bool>(revoker_); }

    bool revoke_dependents(std::uint64_t service_id) {
        if (revoker_) return revoker_(service_id);
        if (!warned_no_revoker_) {
            warned_no_revoker_ = true;
            // A HostCore alone cannot know its components; say so once instead
            // of silently leaving a native consumer holding a dead table.
            core_->log(3, "nostos: a plugin-published service was revoked without a "
                          "dependent-rollback hook; use Host<>::abi_bridge()");
        }
        return false;
    }

    // ---- called by the thunks --------------------------------------------

    void note_publish(std::uint64_t service_id) { published_.push_back(service_id); }

    void forget_publish(std::uint64_t service_id) {
        for (auto it = published_.begin(); it != published_.end(); ++it) {
            if (*it == service_id) {
                published_.erase(it);
                return;
            }
        }
    }

    // Same bookkeeping for subscriptions. The token is what off_raw() needs, and
    // it is the only handle the host has on a raw subscription — a plugin that
    // loses it can never release it, which is exactly why the host keeps its own
    // copy (P2-9 in docs/known-issues.md).
    void note_subscription(std::uint32_t token) {
        if (token != 0) subscriptions_.push_back(token);
    }

    void forget_subscription(std::uint32_t token) {
        for (auto it = subscriptions_.begin(); it != subscriptions_.end(); ++it) {
            if (*it == token) {
                subscriptions_.erase(it);
                return;
            }
        }
    }

private:
    static HostBridge*& current_slot() noexcept {
        thread_local HostBridge* slot = nullptr;
        return slot;
    }

    static const nostos_host_api* host_api_table() noexcept;

    HostCore* core_;
    std::vector<std::uint64_t> published_;
    std::vector<std::uint32_t> subscriptions_;
    std::vector<SessionMark> session_marks_;
    DependentRevoker revoker_;
    HostBridge* previous_ = nullptr;
    bool warned_no_revoker_ = false;

    // poster：deque（引用稳定）让 poster 表里的 self 指针在后续领取时不变；
    // session_posters_ 与 session_marks_ 平行，end_session 时按基准作废。
    std::deque<PosterState> posters_;
    std::vector<PosterState*> session_posters_;
};

namespace detail {

// The frozen table carries no user pointer, so these are the only place the
// current bridge is consulted. All of them are noexcept by construction.
inline const void* service_thunk(std::uint64_t service_id) noexcept {
    HostBridge* bridge = HostBridge::current();
    if (bridge == nullptr) return nullptr;
    try {
        return bridge->core().published().find(service_id);
    } catch (...) {
        return nullptr;
    }
}

inline std::uint32_t on_thunk(std::uint64_t event_id, void (*fn)(void*, const void*),
                             void* user) noexcept {
    HostBridge* bridge = HostBridge::current();
    if (bridge == nullptr || fn == nullptr) return 0;
    try {
        // The subscription outlives this call and may fire from any host
        // stack; it re-installs the bridge itself so a callback that calls
        // back into the host still resolves the right core.
        const std::uint32_t token =
            bridge->core().events().on_raw(event_id, [bridge, fn, user](const void* payload) {
                const HostBridge::Guard guard{*bridge};
                fn(user, payload);
            });
        // Record it, so the host can take it back when the plugin's session ends
        // even if the plugin never calls off(). If recording fails (allocation),
        // undo the subscription: reporting failure while leaving a live callback
        // into the plugin behind would be the worst of both worlds.
        try {
            bridge->note_subscription(token);
        } catch (...) {
            bridge->core().events().off_raw(token);
            return 0;
        }
        return token;
    } catch (...) {
        return 0;
    }
}

inline void off_thunk(std::uint32_t subscription) noexcept {
    HostBridge* bridge = HostBridge::current();
    if (bridge == nullptr) return;
    try {
        bridge->core().events().off_raw(subscription);
        bridge->forget_subscription(subscription);
    } catch (...) {
    }
}

inline void log_thunk(int level, const char* msg) noexcept {
    if (msg == nullptr) return;
    HostBridge* bridge = HostBridge::current();
    if (bridge == nullptr) {  // e.g. a plugin logging from its own thread
        std::fprintf(stderr, "[nostos][%d] %s\n", level, msg);
        return;
    }
    try {
        bridge->core().log(level, msg);
    } catch (...) {
    }
}

inline nostos_status publish_thunk(std::uint64_t service_id, const void* table) noexcept {
    HostBridge* bridge = HostBridge::current();
    if (bridge == nullptr) return NOSTOS_ERR_BAD_STATE;
    // Publishing is legal only inside an activation: that is the scope the host
    // can revoke, and therefore the only scope in which a table owned by plugin
    // code is guaranteed not to outlive the plugin.
    if (!bridge->in_session()) return NOSTOS_ERR_BAD_STATE;
    try {
        bridge->core().published().add(service_id, table, nostos::detail::hex64(service_id));
        bridge->note_publish(service_id);
        return NOSTOS_OK;
    } catch (...) {
        // Duplicate id, unstamped table, allocation failure: all reported the
        // same way because the plugin cannot act differently on any of them.
        return NOSTOS_ERR_INVALID_ARG;
    }
}

inline void unpublish_thunk(std::uint64_t service_id) noexcept {
    HostBridge* bridge = HostBridge::current();
    if (bridge == nullptr) return;
    try {
        bridge->core().published().remove(service_id);
        bridge->forget_publish(service_id);
    } catch (...) {
    }
}

inline void emit_thunk(std::uint64_t event_id, const void* payload) noexcept {
    HostBridge* bridge = HostBridge::current();
    if (bridge == nullptr || payload == nullptr) return;
    try {
        bridge->core().events().emit_raw(event_id, payload);
    } catch (...) {
    }
}

}  // namespace detail
inline const nostos_host_api* HostBridge::host_api_table() noexcept {
    static const nostos_host_api table = {
        static_cast<std::uint32_t>(sizeof(nostos_host_api)),
        NOSTOS_ABI_VERSION,
        &detail::service_thunk,
        &detail::on_thunk,
        &detail::off_thunk,
        &detail::log_thunk,
        &detail::publish_thunk,
        &detail::unpublish_thunk,
        &detail::emit_thunk,
        &HostBridge::acquire_poster_thunk,
    };
    return &table;
}

}  // namespace nostos::abi
