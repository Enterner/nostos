#pragma once
// nostos L1 — typed event bus.
//
// * Identity: an event type E carries `static constexpr std::string_view
//   name`; its fnv1a-64 hash is the event id used by the C++ core and the
//   C ABI alike (see svc_id.hpp).
// * Dispatch policy is chosen at compile time: EmitPolicy (broadcast),
//   BailPolicy (stop at the first listener that returns true), WaterfallPolicy
//   (payload piped through every listener). Note the reach: only EmitPolicy is
//   reachable through the hub — EventHub::bus<E>() creates EventBus<E>, and
//   typed_bus<E>() hands out that same broadcast type — so Host, Context and the
//   C ABI can broadcast and nothing else. Bail and waterfall are for a bus
//   constructed directly (tests/test_event.cpp shows both), which also means
//   they have no raw channel.
// * Reentrancy: dispatch() iterates over a snapshot of the listener list, so
//   listeners may subscribe/unsubscribe/emit freely; an unsubscribe that
//   happens mid-dispatch is honored through the subscription's control block.
//
// Lifetime: a Subscription must not outlive its EventBus. Inside a Host this
// holds by construction (component scopes unwind before the host's hub).

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

// Any type with a static `name` usable as the event's identity.
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

// RAII handle to one registration. Copying shares the registration; the
// destructor and unsubscribe() are idempotent.
class Subscription {
public:
    Subscription() noexcept = default;

    // Internal constructor used by EventBus only: detail-typed parameters
    // keep it out of user code's reach (and friendship out of the headers —
    // constrained class templates cannot be re-declared as friends portably).
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

    // Move assignment is load-bearing, not boilerplate: `sub = bus.on<E>(...)`
    // is the idiomatic form, the right-hand side is an rvalue, so this is the
    // operator that runs. Leaving other.ctrl_ empty is what stops the temporary
    // from unsubscribing the registration the caller just installed — a copy
    // assignment here would share the SubControl and cancel it immediately, with
    // no compile- or run-time diagnostic. Do not "simplify" the two apart.
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

    // Disposable protocol: lets a subscription ride a Scope with
    // scope.own(std::move(sub)). Idempotent, so the destructor is safe too.
    void dispose() noexcept { unsubscribe(); }

    bool active() const noexcept { return ctrl_ && ctrl_->active; }
    explicit operator bool() const noexcept { return active(); }

private:
    std::shared_ptr<detail::SubControl> ctrl_;
    std::function<void()> unlink_;
};

// ---- dispatch policies -------------------------------------------------

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

class EventHub;  // raw seam, defined below

template <typename E, typename Policy = EmitPolicy<E>>
    requires EventType<E>
class EventBus : public detail::BusBase {
public:
    using ListenerT = typename Policy::ListenerT;
    using Result = typename Policy::Result;

    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;  // Subscription::unlink_ captures this

    [[nodiscard]] Subscription on(ListenerT fn) {
        // Copy-on-write: build the next table (dropping slots whose subscription
        // has already ended) and publish it with one assignment. Registration is
        // allowed to allocate — it is not noexcept; dispatch is not, and does not.
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

    // Full policy result (void for EmitPolicy, bool for BailPolicy, the
    // transformed payload for WaterfallPolicy).
    //
    // Cost, measured on this machine (Release, 20k dispatches): the snapshot
    // below is one allocation per dispatch (~34 ns fixed) plus ~11 ns per
    // listener — about 9× an in-place iteration at N=1000, and that is the price
    // of the reentrancy promise above. Keep listener captures small: a listener
    // capturing a large object by value pushes std::function past its
    // small-buffer optimisation, adding one heap allocation per listener per
    // dispatch (measured: 11 allocations/dispatch at N=10 with a 64-byte
    // capture). Capture by reference or by shared_ptr. A copy-on-write slot
    // vector recovers the whole 9× (measured; see P2-6 in
    // docs/known-issues.md) without giving up the promise.
    decltype(auto) dispatch(const E& e) {
        // A shared_ptr copy, not a vector copy: no allocation, and the table a
        // listener sees while running is immutable, so subscribing /
        // unsubscribing / emitting from inside a listener stays safe.
        const auto snapshot = slots_;
        if constexpr (std::is_void_v<Result>) {
            Policy::dispatch(*snapshot, e);
            notify_raw(&e);  // raw listeners see the dispatched event itself
        } else if constexpr (std::is_same_v<Result, bool>) {
            const bool handled = Policy::dispatch(*snapshot, e);
            notify_raw(&e);  // a bail event's payload is the event, not the verdict
            return handled;
        } else {
            const E out = Policy::dispatch(*snapshot, e);
            notify_raw(&out);  // waterfall: raw listeners see the final payload
            return out;
        }
    }

    // Fire-and-forget variant; any policy result is discarded.
    void emit(const E& e) { (void)dispatch(e); }

    // Live subscriptions. Ended ones are inert in the table until the next on()
    // compacts them away, so counting the vector would over-report.
    std::size_t listener_count() const noexcept { return active_count_; }

    const std::type_info& payload_type() const noexcept override { return typeid(E); }

    // Installed by EventHub when the bus is created: every dispatch then also
    // fans out to the hub's raw (type-erased) listeners for this event id.
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

    // A subscription that ends only flips its control block; its table entry
    // becomes inert and is dropped by the next on(), which rebuilds the table
    // anyway. Nothing here allocates — this runs from Subscription::unsubscribe(),
    // which is noexcept, and a noexcept path that allocates turns OOM into
    // terminate().
    void retire() noexcept {
        if (active_count_ > 0) --active_count_;
    }

    // Defined out-of-line, after EventHub is complete.
    void notify_raw(const void* payload);

    std::shared_ptr<const Slots> slots_ = std::make_shared<Slots>();
    std::size_t active_count_ = 0;
    EventHub* hub_ = nullptr;
    std::uint64_t id_ = 0;
};

// Owns one bus per event id, plus the type-erased raw channel the C ABI layer
// plugs into: host_api::on(evt_id, fn, user) becomes on_raw(), and a plugin
// raising an event becomes emit_raw(). Raw listeners always run after the
// typed ones for the same event, in registration order.
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

    // ---- raw channel ------------------------------------------------------

    // fn receives the payload as it exists in this process, valid for the
    // call only. Returns 0 when no listener could be registered.
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

    // A payload that arrived from outside C++ (a plugin emitting a
    // shared-header C struct). Native listeners of E run first — they see the
    // struct as its C++ type — then the raw listeners. Returns false when no
    // native bus for evt_id exists, in which case only raw listeners ran.
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

    // Called from EventBus::notify_raw.
    void dispatch_raw(std::uint64_t evt_id, const void* payload) {
        auto it = raw_.find(evt_id);
        if (it == raw_.end()) return;
        auto snapshot = it->second.slots;  // reentrancy-safe
        for (auto& slot : snapshot)
            if (slot.control->active) slot.fn(payload);
    }

private:
    template <EventType E>
    EventBus<E>& bus() {
        auto [it, inserted] = buses_.try_emplace(event_id<E>, nullptr);
        if (!inserted) {
            // Two payload types sharing one event name would make the raw
            // channel reinterpret memory as the wrong C++ type.
            if (it->second->payload_type() != typeid(E))
                throw event_name_collision(E::name, event_id<E>);
            return *static_cast<EventBus<E>*>(it->second.get());
        }
        auto owned = std::make_unique<EventBus<E>>();
        owned->bind_hub(this, event_id<E>);
        // A raw emit of this id is a payload of the C++ type E (that is the
        // shared-header contract), so route it through the typed bus.
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
