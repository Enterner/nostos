#pragma once
// nostos L1 — Host: activation, transactional rollback, reverse teardown,
// and dynamic (partial) unload.
//
// Host<Cs...> activates components in declaration order. Since a service must
// be provided before it can be acquired (service() throws otherwise), the
// declaration order IS the topological order — no runtime resolver, no cycle
// detection: a cycle would mean "acquire before provide", which simply fails
// and rolls back.
//
// start() is one transaction: if any activate() throws, every component that
// already activated — and the partial state of the failing one — is rolled
// back. shutdown() walks the exact reverse: onStop() → scope unwind (LIFO,
// which also unregisters services) → component destruction.
//
// Phase 2 adds the two halves of the static contract:
//
//   * Compile time (di.hpp): declared Requires must be covered by the union of
//     the declared Provides, with matching C++ types, and every provided
//     svc_id must be unique.
//   * Run time: a service can be revoked while the host runs. unload(i) and
//     revoke(id) roll back the provider *after* rolling back everything that
//     depends on it, dependents first, in reverse activation order — the same
//     global reverse order a full shutdown uses, restricted to the affected
//     closure. Nothing is left holding a service whose provider is gone.

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "nostos/abi/host_api.hpp"
#include "nostos/component.hpp"
#include "nostos/context.hpp"
#include "nostos/di.hpp"
#include "nostos/guard.hpp"
#include "nostos/scope.hpp"

namespace nostos {

template <typename... Cs>
    requires (Component<Cs> && ...)
class Host {
public:
    Host() {
        (check_component<Cs>(), ...);
        check_dependencies();
        register_declarations(std::index_sequence_for<Cs...>{});
    }

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    ~Host() { shutdown(); }

    static constexpr std::size_t component_count = sizeof...(Cs);
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // ---- whole-host lifecycle --------------------------------------------

    // Activate every component that is not active yet, in declaration order,
    // as one transaction. Throws nostos_error when everything is already
    // active. On failure only the components this call activated are rolled
    // back, so start() is safe to retry.
    void start() {
        if constexpr (component_count == 0) {
            started_ = true;
            return;
        } else {
            if (active_components() == component_count)
                throw host_state_error(host_state_error::Reason::already_started);
            std::array<bool, component_count> activated{};
            try {
                activate_missing(activated, std::index_sequence_for<Cs...>{});
            } catch (...) {
                deactivate_marked(activated, std::index_sequence_for<Cs...>{});
                throw;
            }
            started_ = true;
        }
    }

    // Reverse teardown of every active component. noexcept, idempotent.
    void shutdown() noexcept {
        started_ = false;
        deactivate_all();
    }

    // True from a successful start() until shutdown(). A dynamic unload does
    // not clear it — the host is still running, just with fewer components —
    // so start() brings the missing ones back.
    bool started() const noexcept { return started_; }

    // ---- dynamic (partial) unload ----------------------------------------

    // Roll back one component plus every component that (transitively)
    // depends on a service it provides — dependents first, in reverse
    // activation order. Returns how many components were unloaded. noexcept:
    // rollback must not fail. The host stays started.
    std::size_t unload(std::size_t index) noexcept {
        if (index >= component_count) return 0;
        std::array<bool, component_count> mark{};
        mark_with_dependents(index, mark);
        return deactivate_marked(mark, std::index_sequence_for<Cs...>{});
    }

    template <typename C>
    std::size_t unload() noexcept {
        constexpr std::size_t idx = index_of<C>();
        static_assert(idx != npos, "nostos: this component is not part of the host");
        return unload(idx);
    }

    // Revoke a service: unload the component that provides it — and therefore
    // every dependent first. When no component provides id (a published table
    // owned by a dynamic plugin, say) the dependents are still rolled back, so
    // the caller can safely take the provider away; false when nothing is
    // attached to id at all.
    bool revoke(std::uint64_t id) noexcept {
        const std::size_t provider = core_.provider_of(id);
        if (provider == npos) {
            std::array<bool, component_count> mark{};
            if (mark_dependents_of(id, mark) == 0) return false;
            deactivate_marked(mark, std::index_sequence_for<Cs...>{});
            return true;
        }
        unload(provider);
        return true;
    }

    template <fixed_string Name>
    bool revoke() noexcept {
        return revoke(svc_id<Name>);
    }

    // Activate one inactive component: retry after a failed activation, or
    // re-activation of something unload()/revoke() rolled back. Throws the
    // component's own exception, or missing_service when its dependencies are
    // not currently provided; a failure rolls this component back completely.
    void activate(std::size_t index) {
        if (index >= component_count)
            throw std::out_of_range("nostos: component index out of range");
        if (!started_)
            throw host_state_error(host_state_error::Reason::not_started, index);
        if (is_active(index))
            throw host_state_error(host_state_error::Reason::already_active, index);
        activate_at(index, std::index_sequence_for<Cs...>{});
    }

    template <typename C>
    void activate() {
        constexpr std::size_t idx = index_of<C>();
        static_assert(idx != npos, "nostos: this component is not part of the host");
        activate(idx);
    }

    // ---- introspection ---------------------------------------------------

    std::size_t active_components() const noexcept {
        return count_active(std::index_sequence_for<Cs...>{});
    }

    bool is_active(std::size_t index) const noexcept {
        return index < component_count && is_active_at(index, std::index_sequence_for<Cs...>{});
    }

    // The component that currently provides id, or npos.
    std::size_t provider_of(std::uint64_t id) const noexcept { return core_.provider_of(id); }

    ServiceRegistry& services() noexcept { return core_.services(); }
    EventHub& events() noexcept { return core_.events(); }
    Scope& root_scope() noexcept { return core_.root_scope(); }
    Executor& executor() noexcept { return core_.executor(); }
    void set_executor(Executor& e) noexcept { core_.set_executor(e); }

    // poster 队列排水（M1）：宿主主循环每拍调用；在**调用者线程**上、
    // 以各 poster 领取者的桥 Guard 执行已投递任务。未领取过 poster 时为空操作。
    void drain_posted() {
        if (has_abi_bridge()) abi_bridge().drain_posted();
    }

    const std::vector<HostCore::DependencyEdge>& dependency_edges() const noexcept {
        return core_.dependency_edges();
    }

    const std::vector<HostCore::ProvideRecord>& provides() const noexcept {
        return core_.provides();
    }

    PublishedRegistry& published() noexcept { return core_.published(); }
    const PublishedRegistry& published() const noexcept { return core_.published(); }

    // Where nostos_host_api::log lands (default: stderr).
    void set_log_sink(HostCore::LogSink sink) { core_.set_log_sink(std::move(sink)); }

    // Escape hatch: the raw core, for wiring that is not a component.
    HostCore& core() noexcept { return core_; }

    // The C ABI bridge for this host — what a dynamic plugin is handed, wired
    // so that a plugin-published service has its native dependents rolled back
    // before the plugin itself is deactivated. Created on first use (hosts that
    // never load a plugin pay nothing) and installed for this thread, so plugin
    // code can call back into the host from anywhere the host calls it from.
    abi::HostBridge& abi_bridge() {
        if (!bridge_) {
            bridge_ = std::make_unique<abi::HostBridge>(core_);
            bridge_->set_dependent_revoker([this](std::uint64_t id) { return this->revoke(id); });
            bridge_->install();
        }
        return *bridge_;
    }

    // True once abi_bridge() was called at least once.
    bool has_abi_bridge() const noexcept { return bridge_ != nullptr; }

private:
    template <std::size_t I>
    using ComponentI = std::tuple_element_t<I, std::tuple<Cs...>>;

    template <typename C>
    struct Slot {
        std::unique_ptr<C> component;
        std::unique_ptr<Scope> scope;
        bool active = false;
    };

    // ---- compile-time checks ---------------------------------------------

    template <typename C>
    static void check_component() {
        static_assert(Component<C>, "nostos: a Host component must be a default-constructible "
                                    "class with void activate(nostos::Context&)");
        if constexpr (DeclaresOnStop<C>) {
            static_assert(requires(C& c) { { c.onStop() } noexcept; },
                          "nostos: Component::onStop() must be declared noexcept — "
                          "rollback must not fail");
        }
        static_assert(di::well_formed_v<C>,
                      "nostos: a component's Requires/Provides must be std::tuple<Svc<Name, T>...> "
                      "(see nostos/di.hpp)");
    }

    // The host-wide dependency contract: unique Provides, covered Requires.
    // Both failures are reported by instantiating a deliberately failing type
    // whose name carries the offending component and service.
    static void check_dependencies() {
        if constexpr ((di::well_formed_v<Cs> && ...)) {
            using dup_finding = di::duplicate_t<Cs...>;
            using miss_finding = di::unsatisfied_t<Cs...>;
            if constexpr (dup_finding::found)
                (void)sizeof(detail::duplicate_provision_error<typename dup_finding::first,
                                                               typename dup_finding::second>);
            if constexpr (miss_finding::found)
                (void)sizeof(detail::unsatisfied_requirement_error<typename miss_finding::plugin,
                                                                   typename miss_finding::service,
                                                                   typename miss_finding::provided>);
        }
    }

    // Hand the declarations to the core so Context can refuse what the
    // component's own contract does not cover: provide() against a missing
    // Provides entry, and service()/require() against a missing — or
    // differently-typed — Requires entry.
    template <std::size_t... I>
    void register_declarations(std::index_sequence<I...>) {
        (core_.declare_provides(I, declared_ids<ComponentI<I>>()), ...);
        (core_.declare_requires(I, declared_require_types<ComponentI<I>>()), ...);
    }

    template <typename C>
    static std::vector<std::uint64_t> declared_ids() {
        if constexpr (di::well_formed_v<C>) {
            using list = di::id_list<di::provides_t<C>>;
            return std::vector<std::uint64_t>(list::value.begin(), list::value.end());
        } else {
            return {};
        }
    }

    // Requires entries carry their C++ type as well: the call site names the
    // type independently of the declaration, so id equality alone cannot catch
    // a drifted acquisition.
    template <typename List>
    struct DeclaredRequires;
    template <typename... S>
    struct DeclaredRequires<std::tuple<S...>> {
        static std::vector<HostCore::DeclaredRequire> make() {
            return std::vector<HostCore::DeclaredRequire>{
                HostCore::DeclaredRequire{S::id, &typeid(typename S::type)}...};
        }
    };

    template <typename C>
    static std::vector<HostCore::DeclaredRequire> declared_require_types() {
        if constexpr (di::well_formed_v<C>) {
            return DeclaredRequires<di::requires_t<C>>::make();
        } else {
            return {};
        }
    }

    template <typename C, std::size_t I = 0>
    static constexpr std::size_t index_of() noexcept {
        if constexpr (I >= component_count) {
            return npos;
        } else if constexpr (std::is_same_v<C, ComponentI<I>>) {
            return I;
        } else {
            return index_of<C, I + 1>();
        }
    }

    // ---- activation -------------------------------------------------------

    template <std::size_t I>
    bool slot_active() const noexcept {
        return std::get<I>(slots_).active;
    }

    template <std::size_t... I>
    void activate_missing(std::array<bool, component_count>& activated, std::index_sequence<I...>) {
        ((slot_active<I>() ? (void)0 : (void)(activate_one<I>(), activated[I] = true)), ...);
    }

    template <std::size_t... I>
    void activate_at(std::size_t index, std::index_sequence<I...>) {
        ((index == I ? (activate_one<I>(), 0) : 0), ...);
    }

    template <std::size_t I>
    void activate_one() {
        using C = ComponentI<I>;
        auto& slot = std::get<I>(slots_);
        slot.component = std::make_unique<C>();
        slot.scope = std::make_unique<Scope>();
        try {
            Context ctx{core_, I, *slot.scope};
            slot.component->activate(ctx);
        } catch (...) {
            core_.clear_dependencies_for(I);
            core_.clear_provides_for(I);
            slot.scope->reset();  // transactional: undo everything activate() did
            slot.component.reset();
            slot.scope.reset();
            throw;
        }
        slot.active = true;
    }

    // ---- teardown ---------------------------------------------------------

    void deactivate_all() noexcept { deactivate_all(std::index_sequence_for<Cs...>{}); }

    template <std::size_t... I>
    void deactivate_all(std::index_sequence<I...>) noexcept {
        (deactivate_one<component_count - 1 - I>(), ...);  // reverse activation order
    }

    // Deactivate the marked components, still in reverse activation order.
    template <std::size_t... I>
    std::size_t deactivate_marked(const std::array<bool, component_count>& mark,
                                  std::index_sequence<I...>) noexcept {
        std::size_t count = 0;
        (deactivate_marked_one<component_count - 1 - I>(mark, count), ...);
        return count;
    }

    template <std::size_t I>
    void deactivate_marked_one(const std::array<bool, component_count>& mark,
                               std::size_t& count) noexcept {
        if (mark[I] && slot_active<I>()) {
            deactivate_one<I>();
            ++count;
        }
    }

    template <std::size_t I>
    void deactivate_one() noexcept {
        auto& slot = std::get<I>(slots_);
        if (!slot.active) return;
        slot.active = false;
        using C = ComponentI<I>;
        if constexpr (DeclaresOnStop<C>) slot.component->onStop();
        core_.clear_dependencies_for(I);
        slot.scope->reset();  // LIFO: user cleanup first, service unregistration last
        core_.clear_provides_for(I);
        slot.component.reset();
        slot.scope.reset();
    }

    // Every component that (transitively) depends on a service the root
    // provides, plus the root itself. Fixed point over the recorded edges:
    // the edge set is tiny and the core is single-threaded, so a simple
    // loop beats a graph structure — and it cannot allocate, which matters
    // because unload() is noexcept.
    void mark_with_dependents(std::size_t root,
                              std::array<bool, component_count>& mark) const noexcept {
        mark[root] = true;
        mark_closure(mark);
    }

    // Every component that (transitively) depends on `id`, whoever provides it.
    // Used for services no component owns — a dynamic plugin's published
    // tables — where there is no provider to unload but the dependents still
    // have to roll back before the table disappears. Returns how many were
    // newly marked.
    std::size_t mark_dependents_of(std::uint64_t id,
                                   std::array<bool, component_count>& mark) const noexcept {
        std::size_t count = 0;
        for (const auto& edge : core_.dependency_edges()) {
            if (edge.service_id != id || edge.consumer >= component_count) continue;
            if (!mark[edge.consumer]) {
                mark[edge.consumer] = true;
                ++count;
            }
        }
        mark_closure(mark, &count);
        return count;
    }

    // Propagate marks along the dependency edges to a fixed point: if a marked
    // component provides a service, everyone who acquired it goes too.
    void mark_closure(std::array<bool, component_count>& mark,
                      std::size_t* count = nullptr) const noexcept {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& edge : core_.dependency_edges()) {
                if (edge.consumer >= component_count) continue;
                if (mark[edge.consumer]) continue;
                const std::size_t provider = core_.provider_of(edge.service_id);
                if (provider != npos && mark[provider]) {
                    mark[edge.consumer] = true;
                    if (count != nullptr) ++*count;
                    changed = true;
                }
            }
        }
    }

    // ---- counting ---------------------------------------------------------

    template <std::size_t... I>
    std::size_t count_active(std::index_sequence<I...>) const noexcept {
        return (std::size_t{0} + ... + (slot_active<I>() ? std::size_t{1} : std::size_t{0}));
    }

    template <std::size_t... I>
    bool is_active_at(std::size_t index, std::index_sequence<I...>) const noexcept {
        return ((index == I ? slot_active<I>() : false) || ...);
    }

    HostCore core_;  // declared first → destroyed last (components reference it)
    std::tuple<Slot<Cs>...> slots_;
    std::unique_ptr<abi::HostBridge> bridge_;
    bool started_ = false;
};

}  // namespace nostos
