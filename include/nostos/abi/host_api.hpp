#pragma once
// nostos L2 —— C ABI 边界的宿主侧（docs/design.md §4.7）。
//
// HostBridge 把 HostCore 变成 plugin 拿到的无状态 nostos_host_api 表。每个
// thunk 都是 noexcept 并吞掉异常：任何东西都不得跨越边界（纪律 2），宿主侧
// 的失败也不例外。
//
// 冻结的表没有用户指针，所以 thunk 经一个线程本地的*当前桥*来解析“是哪个
// 宿主？”。Host<...> 在创建时安装自己的桥（install()），这意味着 plugin 代码
// 可以在该线程的任何位置回调宿主——activate() 期间、在被宿主调用的
// published 表函数内部、在事件回调里——而不必让每个原生调用者都记住一个
// guard。Guard 是显式的、带作用域的形式（loader 在每次调用 plugin 代码时
// 都用它），也是刻意嵌套两个桥的方式。
//
// 设计使然的后果：每个线程由一个宿主独占 ABI。plugin 在宿主从未调用过它的
// 线程上调用，得到的是 NULL/0 而不是错误的宿主；同一线程上构造的两个宿主
// 解析到最近创建的那个（core 是单线程的，docs/design.md §4.8）。
//
// 发布会话：plugin 在 begin_session() 与 end_session() 之间发布的一切都会
// 被记录，因此 plugin 拥有的表绝不可能比 plugin 的激活活得更久——无论是
// activate 失败，还是 plugin 忘了 unpublish。这些表消失之前，*依赖者*会经
// Host<...> 安装的 revoker 钩子回滚。

#include <atomic>
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

    // 从未 install 的桥解析不出任何东西——即 thunk 报告为“没有宿主”的空情形。
    ~HostBridge() {
        if (current() == this) set_current(previous_);
    }

    HostCore& core() noexcept { return *core_; }

    // 交给 plugin 的表。其地址在进程内稳定，且每个字段——包括 v1 冻结之后
    // 追加的字段——都已填充。
    static const nostos_host_api* api() noexcept { return host_api_table(); }

    // ---- 当前桥 ------------------------------------------------------------

    static HostBridge* current() noexcept { return current_slot(); }
    static void set_current(HostBridge* bridge) noexcept { current_slot() = bridge; }

    // 让本桥成为 thunk 在本线程上解析到的那个桥，直到它被销毁或安装了另一个
    // 桥。幂等。由 Host<...> 在首次需要自己的桥时调用。
    void install() noexcept {
        if (current() == this) return;
        previous_ = current();
        set_current(this);
    }

    // RAII 安装/恢复，无分配，因此在 noexcept thunk 内使用是安全的。plugin
    // 回调经由自己的 Guard 重入。
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

    // ---- 发布 / 订阅会话 ---------------------------------------------------

    // 会话到目前为止新增的内容。两半都追踪是同一个原因：宿主必须能收回
    // plugin 交出的东西。
    struct SessionMark {
        std::size_t published = 0;
        std::size_t subscriptions = 0;
        std::size_t posters = 0;
    };

    void begin_session() {
        session_marks_.push_back(
            SessionMark{published_.size(), subscriptions_.size(), session_posters_.size()});
    }

    // 回滚依赖当前会话所发布服务的原生组件——依赖者先行，在 plugin 本体
    // deactivate 之前。返回回滚过依赖者的服务个数。
    std::size_t revoke_session_dependents() {
        std::size_t count = 0;
        const std::size_t mark = session_marks_.empty() ? 0 : session_marks_.back().published;
        for (std::size_t i = published_.size(); i > mark; --i)
            if (revoke_dependents(published_[i - 1])) ++count;
        return count;
    }

    // 忘掉（并撤销）当前会话发布的一切，并退订 plugin 在会话期间订阅的一切。
    // noexcept：它运行在收尾路径上，包括激活失败的路径。
    void end_session() noexcept {
        if (session_marks_.empty()) return;
        const SessionMark mark = session_marks_.back();
        session_marks_.pop_back();
        for (std::size_t i = published_.size(); i > mark.published; --i)
            core_->published().remove(published_[i - 1]);
        published_.resize(mark.published);

        // 订阅这一半。plugin 把回调交给宿主却从不收回，宿主就会继续向一个
        // 即将 unmap 的模块分发——而且与 published 表（宿主按需解析）不同，
        // 订阅是指向 plugin 代码的活指针。最外层会话结束时 plugin 将彻底
        // 消失，所以它在会话之外订阅的一切也一并退订。
        const std::size_t from = session_marks_.empty() ? 0 : mark.subscriptions;
        for (std::size_t i = subscriptions_.size(); i > from; --i)
            core_->events().off_raw(subscriptions_[i - 1]);
        subscriptions_.resize(from);

        // 投递器这一半。本会话内取用的 poster 停止接收任务：plugin 若在
        // deactivate 之后仍用自己的线程投递，宿主就会在收尾之后继续执行
        // plugin 代码。
        // F11：作废前先走“丢弃回调”——会话内入队成功但未排水的任务，逐条交还
        // 投递方回收（fn 必须是静态的、只触碰 arg：此刻插件对象已析构，代码段
        // 仍映射到 unload 为止）。未注册 on_drop ⇒ 静默丢弃。
        const std::size_t poster_from = session_marks_.empty() ? 0 : mark.posters;
        for (std::size_t i = session_posters_.size(); i > poster_from; --i) {
            PosterState* ps = session_posters_[i - 1];
            std::vector<std::pair<void (*)(void*), void*>> dropped;
            void (*drop)(void*) = nullptr;
            {
                std::lock_guard<std::mutex> lock(ps->mu);
                ps->live = false;
                dropped.swap(ps->pending);
                drop = ps->drop;
            }
            if (drop != nullptr)
                for (auto& task : dropped) {
                    const HostBridge::Guard guard{*this};
                    drop(task.second);  // 约定：fn 不抛（宿主侧再兜底一层）
                }
        }
        session_posters_.resize(poster_from);
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
    // 本桥已交出的、仍然存活的订阅（无论是否在会话内）。
    std::size_t subscriptions_tracked() const noexcept { return subscriptions_.size(); }

    // ---- poster：跨线程投递（绑定桥；M1，docs/proposal-kit.md §6.1）--------

    // poster 的宿主侧状态。表指向它（桥所有 ⇒ 表随桥存活）；
    // pending 是线程安全的待执行队列，宿主主循环 drain_posted() 排水。
    struct PosterState {
        HostBridge* bridge = nullptr;
        std::mutex mu;
        std::vector<std::pair<void (*)(void*), void*>> pending;
        bool live = true;
        void (*drop)(void*) = nullptr;  // 会话作废时未执行任务的逐条回收回调（可空；F11）
    };

    // 表槽位（static：表构造器在类外引用）。自带锁；会话作废后返回 BAD_STATE。
    static nostos_status post_thunk(void* self, void (*fn)(void*), void* arg) noexcept {
        auto* ps = static_cast<PosterState*>(self);
        if (ps == nullptr || fn == nullptr) return NOSTOS_ERR_INVALID_ARG;
        {
            std::lock_guard<std::mutex> lock(ps->mu);
            if (!ps->live) return NOSTOS_ERR_BAD_STATE;
            ps->pending.push_back({fn, arg});
        }
        // 唤醒接缝：锁外触发（hook 只做唤醒；即便 hook 里就地排水也不会死锁——
        // drain_posted 自己上锁）。见 set_post_wake。
        if (ps->bridge != nullptr) {
            if (auto* wake = ps->bridge->wake_fn_.load(std::memory_order_acquire))
                wake(ps->bridge->wake_arg_.load(std::memory_order_acquire));
        }
        return NOSTOS_OK;
    }

    // 表槽位（static）：注册“丢弃回调”（F11：post 成功但会话作废时未执行的任务，
    // 作废时逐条回调 fn(arg)，投递方就地回收——未注册则静默丢弃）。
    static nostos_status on_drop_thunk(void* self, void (*fn)(void* arg)) noexcept {
        auto* ps = static_cast<PosterState*>(self);
        if (ps == nullptr || fn == nullptr) return NOSTOS_ERR_INVALID_ARG;
        std::lock_guard<std::mutex> lock(ps->mu);
        ps->drop = fn;  // 同一 poster 反复注册以最后一次为准（契约明示）
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

    // ---- post 唤醒接缝 ------------------------------------------------------

    // post 成功入队后，在**投递线程上**触发一次 hook（若已设置）。宿主用它从
    // 阻塞等待（事件/信号量）中醒来排水，替代固定节奏的轮询——轮询要低延迟
    // 就得高频空转，要省电就得拉长周期；不设置则维持轮询模式，现有宿主零改动。
    // 约定：hook 必须 noexcept 且轻（只做“唤醒”，重活留给排水后的任务体）；
    // 宿主应在装载插件前设置一次——运行期更换不作同步承诺（投递线程可能读到
    // 旧组合；构造性规避：只置一次）。
    void set_post_wake(void (*fn)(void*), void* arg) noexcept {
        wake_fn_.store(fn, std::memory_order_release);
        wake_arg_.store(arg, std::memory_order_release);
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
        out->on_drop = &on_drop_thunk;
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

    // ---- 依赖者回滚钩子 ----------------------------------------------------

    // 由 Host<...> 安装：只有它了解自己的组件，因此只有它能回滚取用了
    // published 表的那些组件。发生了回滚时返回 true。
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
            // 裸的 HostCore 无从了解自己的组件；明说一次，而不是让原生消费者
            // 无声地攥着一张死表。
            core_->log(3, "nostos: a plugin-published service was revoked without a "
                          "dependent-rollback hook; use Host<>::abi_bridge()");
        }
        return false;
    }

    // ---- 由 thunk 调用 ----------------------------------------------------

    void note_publish(std::uint64_t service_id) { published_.push_back(service_id); }

    void forget_publish(std::uint64_t service_id) {
        for (auto it = published_.begin(); it != published_.end(); ++it) {
            if (*it == service_id) {
                published_.erase(it);
                return;
            }
        }
    }

    // 订阅的同一套记账。token 是 off_raw() 所需的东西，也是宿主对裸订阅持有
    // 的唯一句柄——丢掉它的 plugin 永远无法释放该订阅，这正是宿主自己留一份
    // 拷贝的原因（docs/known-issues.md 的 P2-9）。
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

    // 唤醒接缝（见 set_post_wake）。atomic：post 来自任意插件的任意线程。
    std::atomic<void (*)(void*)> wake_fn_{nullptr};
    std::atomic<void*> wake_arg_{nullptr};
};

namespace detail {

// 冻结的表不带用户指针，因此这几处是仅有的查询当前桥的地方。它们在构造上
// 都是 noexcept。
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
        // 订阅比本次调用活得更久，可能从任何宿主栈上触发；它会自行重装桥，
        // 于是回调里再调宿主仍能解析到正确的 core。
        const std::uint32_t token =
            bridge->core().events().on_raw(event_id, [bridge, fn, user](const void* payload) {
                const HostBridge::Guard guard{*bridge};
                fn(user, payload);
            });
        // 记下它，plugin 的会话结束时宿主才能收回——哪怕 plugin 从不调用
        // off()。若记账失败（分配），则撤销订阅：一边报告失败一边留下指向
        // plugin 的活回调，是两头都糟的做法。
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
    if (bridge == nullptr) {  // 例如 plugin 在自己的线程上打日志
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
    // 发布只在激活内部合法：那是宿主能撤销的范围，因此也是唯一能保证
    // plugin 代码拥有的表不比 plugin 活得更久的范围。
    if (!bridge->in_session()) return NOSTOS_ERR_BAD_STATE;
    try {
        bridge->core().published().add(service_id, table, nostos::detail::hex64(service_id));
        bridge->note_publish(service_id);
        return NOSTOS_OK;
    } catch (...) {
        // id 重复、表未盖章、分配失败：一律同样上报，因为 plugin 对其中任何
        // 一种都无法采取不同的应对。
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

inline uint32_t published_list_thunk(struct nostos_published_entry* out, uint32_t cap) noexcept {
    HostBridge* bridge = HostBridge::current();
    if (bridge == nullptr) return 0;
    try {
        const auto& registry = bridge->core().published();
        const auto& ids = registry.ids();
        if (out == nullptr || cap == 0) return static_cast<uint32_t>(ids.size());
        const uint32_t count = cap < ids.size() ? cap : static_cast<uint32_t>(ids.size());
        for (uint32_t i = 0; i < count; ++i) {
            out[i].svc_id = ids[i];
            // 名字指向注册表内部存储：随条目存活（unpublish / 重新发布后失效）。
            const std::string* name = registry.name_of(ids[i]);
            out[i].name = name != nullptr ? name->c_str() : "";
        }
        return count;
    } catch (...) {
        return 0;
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
        &detail::published_list_thunk,
    };
    return &table;
}

}  // namespace nostos::abi
