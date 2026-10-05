#pragma once
// nostos L1 —— 类型化 event bus。
//
// * 身份：event 类型 E 携带 `static constexpr std::string_view name`；其
//   fnv1a-64 哈希就是 event id，C++ 核心与 C ABI 通用（见 svc_id.hpp）。
// * dispatch 策略在编译期选定：EmitPolicy（广播）、BailPolicy（在第一个返回
//   true 的 listener 处停下）、WaterfallPolicy（载荷逐个流经所有 listener）。
//   注意可达范围：经由 hub 只能到达 EmitPolicy——EventHub::bus<E>() 创建
//   EventBus<E>，typed_bus<E>() 发出的也是同一个广播类型——因此 Host、
//   Context 与 C ABI 能做的只有广播。bail 与 waterfall 供直接构造的 bus 使用
//   （tests/test_event.cpp 两者都有演示），这也意味着它们没有 raw 通道。
// * 重入：dispatch() 在 listener 列表的快照上迭代，因此 listener 可以随意
//   subscribe/unsubscribe/emit；dispatch 中途发生的 unsubscribe 经订阅的
//   控制块照样生效。
//
// 生存期：Subscription 不得比它的 EventBus 活得久。在 Host 内部这由构造方式
// 保证（component 的 scope 先于 host 的 hub unwind）。

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nostos/error.hpp"
#include "nostos/svc_id.hpp"

namespace nostos {

// 任何带有可用作 event 身份的静态 `name` 的类型。
template <typename E>
concept EventType = requires { { E::name } -> std::convertible_to<std::string_view>; };

template <EventType E>
inline constexpr std::uint64_t event_id = detail::fnv1a64(std::string_view{E::name});

template <EventType E>
using Listener = std::function<void(const E&)>;

namespace detail {

struct SubControl {
    bool active = true;
};

class BusBase {
public:
    virtual ~BusBase() = default;
    virtual const std::type_info& payload_type() const noexcept = 0;
};

}  // namespace detail

// 一条注册的 RAII 句柄。拷贝即共享同一注册；析构与 unsubscribe() 幂等。
class Subscription {
public:
    Subscription() noexcept = default;

    // 仅供 EventBus 使用的内部构造函数：detail 类型的参数让用户代码够不着它
    // （也让友元声明不必进头文件——受约束的类模板无法以可移植的方式重新声明
    // 为友元）。
    Subscription(std::shared_ptr<detail::SubControl> ctrl, std::function<void()> unlink)
        : ctrl_(std::move(ctrl)), unlink_(std::move(unlink)) {}

    Subscription(const Subscription&) = default;
    Subscription(Subscription&&) noexcept = default;

    Subscription& operator=(const Subscription& other) {
        if (this != &other) {
            unsubscribe();
            ctrl_ = other.ctrl_;
            unlink_ = other.unlink_;
        }
        return *this;
    }

    // 移动赋值承重，不是样板：`sub = bus.on<E>(...)` 是惯用写法，右侧是右值，
    // 跑的正是这个运算符。让 other.ctrl_ 留空，临时对象才不会把调用方刚装上的
    // 注册退订掉——这里若用拷贝赋值，双方共享 SubControl，注册会被立即取消，
    // 且编译期、运行期都没有任何诊断。别以"简化"为名把两者改掉。
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            unsubscribe();
            ctrl_ = std::move(other.ctrl_);
            unlink_ = std::move(other.unlink_);
        }
        return *this;
    }

    ~Subscription() { unsubscribe(); }

    void unsubscribe() noexcept {
        if (ctrl_ && ctrl_->active) {
            ctrl_->active = false;
            if (unlink_) unlink_();
        }
        ctrl_.reset();
        unlink_ = nullptr;
    }

    // Disposable 协议：让订阅能搭乘 Scope——scope.own(std::move(sub))。幂等，
    // 因此析构函数也安全。
    void dispose() noexcept { unsubscribe(); }

    bool active() const noexcept { return ctrl_ && ctrl_->active; }
    explicit operator bool() const noexcept { return active(); }

private:
    std::shared_ptr<detail::SubControl> ctrl_;
    std::function<void()> unlink_;
};

// ---- dispatch 策略 ------------------------------------------------------

template <typename E>
struct EmitPolicy {
    using ListenerT = std::function<void(const E&)>;
    using Result = void;

    template <typename Slots>
    static void dispatch(const Slots& slots, const E& e) {
        for (const auto& s : slots)
            if (s.control->active) s.fn(e);
    }
};

template <typename E>
struct BailPolicy {
    using ListenerT = std::function<bool(const E&)>;
    using Result = bool;

    template <typename Slots>
    static bool dispatch(const Slots& slots, const E& e) {
        for (const auto& s : slots)
            if (s.control->active && s.fn(e)) return true;
        return false;
    }
};

template <typename E>
struct WaterfallPolicy {
    using ListenerT = std::function<E(const E&)>;
    using Result = E;

    template <typename Slots>
    static E dispatch(const Slots& slots, E e) {
        for (const auto& s : slots)
            if (s.control->active) e = s.fn(e);
        return e;
    }
};

// ------------------------------------------------------------------------

class EventHub;  // raw 接缝，定义在下面

template <typename E, typename Policy = EmitPolicy<E>>
    requires EventType<E>
class EventBus : public detail::BusBase {
public:
    using ListenerT = typename Policy::ListenerT;
    using Result = typename Policy::Result;

    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;  // Subscription::unlink_ 捕获了 this

    [[nodiscard]] Subscription on(ListenerT fn) {
        // copy-on-write：先构建下一张表（丢弃订阅已结束的槽位），再用一次
        // 赋值发布。注册允许分配——它不是 noexcept；dispatch 则不允许、也不会
        // 分配。
        auto next = std::make_shared<Slots>();
        next->reserve(active_count_ + 1);
        for (const auto& s : *slots_)
            if (s.control->active) next->push_back(s);
        auto control = std::make_shared<detail::SubControl>();
        next->push_back(Slot{std::move(fn), control});
        slots_ = std::move(next);
        ++active_count_;
        return Subscription(std::move(control), [this] { retire(); });
    }

    // 完整的策略结果（EmitPolicy 为 void，BailPolicy 为 bool，WaterfallPolicy
    // 为变换后的载荷）。
    //
    // 本机实测开销（Release，2 万次 dispatch）：下面的快照是每次 dispatch 一次
    // 分配（固定约 34 ns）加每个 listener 约 11 ns——N=1000 时约为原地迭代的
    // 9 倍，这就是上面重入承诺的代价。listener 的捕获要保持小：按值捕获大对象
    // 会把 std::function 顶出小缓冲区优化，每个 listener 每次 dispatch 多一次
    // 堆分配（实测：N=10、64 字节捕获时每次 dispatch 11 次分配）。按引用或
    // shared_ptr 捕获。换成 copy-on-write 的槽位向量能收回全部 9 倍（实测；
    // 见 docs/known-issues.md 的 P2-6），且不放弃重入承诺。
    decltype(auto) dispatch(const E& e) {
        // 拷贝的是 shared_ptr 而非 vector：零分配，且 listener 运行期间看到的
        // 表不可变，因此在 listener 内部 subscribe / unsubscribe / emit 始终
        // 安全。
        const auto snapshot = slots_;
        if constexpr (std::is_void_v<Result>) {
            Policy::dispatch(*snapshot, e);
            notify_raw(&e);  // raw listener 看到的就是被派发的这个 event 本身
        } else if constexpr (std::is_same_v<Result, bool>) {
            const bool handled = Policy::dispatch(*snapshot, e);
            notify_raw(&e);  // bail event 的载荷是 event 本身，而非裁定结果
            return handled;
        } else {
            const E out = Policy::dispatch(*snapshot, e);
            notify_raw(&out);  // waterfall：raw listener 看到的是最终载荷
            return out;
        }
    }

    // fire-and-forget 变体；任何策略结果都被丢弃。
    void emit(const E& e) { (void)dispatch(e); }

    // 存活的订阅数。已结束的订阅在表中保持惰性，直到下一次 on() 重建表时才被
    // 顺带清除，所以直接数向量会虚报。
    std::size_t listener_count() const noexcept { return active_count_; }

    const std::type_info& payload_type() const noexcept override { return typeid(E); }

    // 由 EventHub 在创建 bus 时安装：此后每次 dispatch 都会向 hub 的 raw
    // （类型擦除）listener 按该 event id 扇出。
    void bind_hub(EventHub* hub, std::uint64_t id) noexcept {
        hub_ = hub;
        id_ = id;
    }

private:
    struct Slot {
        ListenerT fn;
        std::shared_ptr<detail::SubControl> control;
    };
    using Slots = std::vector<Slot>;

    // 订阅结束时只翻转自己的控制块；其表项随之惰性化，由下一次 on() 重建表时
    // 顺带丢弃。这里不允许任何分配——它从 Subscription::unsubscribe() 进来，
    // 那是 noexcept 的，而会分配的 noexcept 路径会把 OOM 变成 terminate()。
    void retire() noexcept {
        if (active_count_ > 0) --active_count_;
    }

    // 留到 EventHub 完整之后在类外定义。
    void notify_raw(const void* payload);

    std::shared_ptr<const Slots> slots_ = std::make_shared<Slots>();
    std::size_t active_count_ = 0;
    EventHub* hub_ = nullptr;
    std::uint64_t id_ = 0;
};

// 每个 event id 拥有一条 bus，外加供 C ABI 层接入的类型擦除 raw 通道：
// host_api::on(evt_id, fn, user) 变成 on_raw()，插件引发 event 变成
// emit_raw()。同一个 event 的 raw listener 总是在类型化 listener 之后、按
// 注册顺序运行。
class EventHub {
public:
    EventHub() = default;
    EventHub(const EventHub&) = delete;
    EventHub& operator=(const EventHub&) = delete;

    template <EventType E, typename F>
        requires std::invocable<std::decay_t<F>&, const E&>
    [[nodiscard]] Subscription on(F&& fn) {
        return bus<E>().on(Listener<E>(std::forward<F>(fn)));
    }

    template <EventType E>
    void emit(const E& e) {
        bus<E>().emit(e);
    }

    template <EventType E>
    EventBus<E>& typed_bus() {
        return bus<E>();
    }

    // ---- raw 通道 ----------------------------------------------------------

    // fn 收到的载荷是它在本进程中的原样存在，调用期有效。无法注册 listener
    // 时返回 0。
    [[nodiscard]] std::uint32_t on_raw(std::uint64_t evt_id, std::function<void(const void*)> fn) {
        if (!fn) return 0;
        const std::uint32_t token = next_raw_token_++;
        auto control = std::make_shared<detail::SubControl>();
        raw_[evt_id].slots.push_back(RawSlot{token, std::move(fn), std::move(control)});
        raw_tokens_.emplace(token, evt_id);
        return token;
    }

    void off_raw(std::uint32_t token) noexcept {
        if (token == 0) return;
        auto t = raw_tokens_.find(token);
        if (t == raw_tokens_.end()) return;
        const std::uint64_t id = t->second;
        raw_tokens_.erase(t);
        auto channel = raw_.find(id);
        if (channel == raw_.end()) return;
        auto& slots = channel->second.slots;
        for (auto it = slots.begin(); it != slots.end(); ++it) {
            if (it->token == token) {
                if (it->control) it->control->active = false;
                slots.erase(it);
                break;
            }
        }
        if (slots.empty()) raw_.erase(channel);
    }

    // 从 C++ 之外抵达的载荷（插件 emit 一个共享头里的 C 结构体）。E 的原生
    // listener 先跑——它们把该结构体当作自己的 C++ 类型来看——然后是 raw
    // listener。evt_id 没有原生 bus 时返回 false，此时只有 raw listener 跑过。
    bool emit_raw(std::uint64_t evt_id, const void* payload) {
        auto emitter = emitters_.find(evt_id);
        if (emitter == emitters_.end()) {
            dispatch_raw(evt_id, payload);
            return false;
        }
        emitter->second(payload);
        return true;
    }

    bool knows(std::uint64_t evt_id) const noexcept { return buses_.count(evt_id) != 0; }

    std::size_t raw_listener_count(std::uint64_t evt_id) const noexcept {
        auto it = raw_.find(evt_id);
        return it == raw_.end() ? 0 : it->second.slots.size();
    }

    std::size_t raw_channel_count() const noexcept { return raw_.size(); }

    // 由 EventBus::notify_raw 调用。
    void dispatch_raw(std::uint64_t evt_id, const void* payload) {
        auto it = raw_.find(evt_id);
        if (it == raw_.end()) return;
        auto snapshot = it->second.slots;  // 重入安全
        for (auto& slot : snapshot)
            if (slot.control->active) slot.fn(payload);
    }

private:
    template <EventType E>
    EventBus<E>& bus() {
        auto [it, inserted] = buses_.try_emplace(event_id<E>, nullptr);
        if (!inserted) {
            // 两个载荷类型共用一个 event 名，会让 raw 通道把内存重解释成错误
            // 的 C++ 类型。
            if (it->second->payload_type() != typeid(E))
                throw event_name_collision(E::name, event_id<E>);
            return *static_cast<EventBus<E>*>(it->second.get());
        }
        auto owned = std::make_unique<EventBus<E>>();
        owned->bind_hub(this, event_id<E>);
        // 对该 id 的 raw emit 就是一个 C++ 类型 E 的载荷（共享头契约正是如此），
        // 因此把它路由进类型化 bus。
        emitters_.emplace(event_id<E>, [bus_ptr = owned.get()](const void* payload) {
            bus_ptr->emit(*static_cast<const E*>(payload));
        });
        it->second = std::move(owned);
        return *static_cast<EventBus<E>*>(it->second.get());
    }

    struct RawSlot {
        std::uint32_t token;
        std::function<void(const void*)> fn;
        std::shared_ptr<detail::SubControl> control;
    };

    struct RawChannel {
        std::vector<RawSlot> slots;
    };

    std::unordered_map<std::uint64_t, std::unique_ptr<detail::BusBase>> buses_;
    std::unordered_map<std::uint64_t, std::function<void(const void*)>> emitters_;
    std::unordered_map<std::uint64_t, RawChannel> raw_;
    std::unordered_map<std::uint32_t, std::uint64_t> raw_tokens_;
    std::uint32_t next_raw_token_ = 1;
};

template <typename E, typename Policy>
    requires EventType<E>
void EventBus<E, Policy>::notify_raw(const void* payload) {
    if (hub_ != nullptr) hub_->dispatch_raw(id_, payload);
}

}  // namespace nostos
