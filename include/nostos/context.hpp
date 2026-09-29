#pragma once
// nostos L1 — Context (the activation-time API) and HostCore (the
// non-template machinery a Host sits on).
//
// Context is handed to Component::activate(). Its shape encodes the two core
// invariants of the design:
//
// * 取用即依赖 — service<T>() records a dependency edge at the moment of
//   acquisition; no separate graph declaration exists.
// * 一切副作用经 scope() — every reversible side effect a component creates
//   is owned by its Scope, so an abnormal exit from activate() rolls the
//   component back automatically (activation = transaction).

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include "nostos/event.hpp"
#include "nostos/executor.hpp"
#include "nostos/guard.hpp"
#include "nostos/published.hpp"
#include "nostos/registry.hpp"
#include "nostos/scope.hpp"
#include "nostos/svc_id.hpp"

namespace nostos {

class HostCore {
public:
    HostCore() = default;
    HostCore(const HostCore&) = delete;
    HostCore& operator=(const HostCore&) = delete;

    ServiceRegistry& services() noexcept { return services_; }
    EventHub& events() noexcept { return events_; }
    Scope& root_scope() noexcept { return root_; }

    Executor& executor() noexcept { return *executor_; }
    void set_executor(Executor& e) noexcept { executor_ = &e; }  // caller keeps it alive

    struct DependencyEdge {
        std::size_t consumer;  // activation index of the acquiring component
        std::uint64_t service_id;
        std::string service_name;
    };

    // Which component currently provides which service. The registry itself
    // is keyed by svc_id and knows nothing about components; this table is
    // what makes "unload the provider of X" possible (see Host::revoke).
    struct ProvideRecord {
        std::size_t provider;  // activation index of the providing component
        std::uint64_t service_id;
        std::string service_name;
    };

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    void record_dependency(std::size_t consumer, std::uint64_t id, std::string name) {
        edges_.push_back(DependencyEdge{consumer, id, std::move(name)});
    }

    void clear_dependencies_for(std::size_t consumer) {
        edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                    [&](const DependencyEdge& e) { return e.consumer == consumer; }),
                     edges_.end());
    }

    const std::vector<DependencyEdge>& dependency_edges() const noexcept { return edges_; }

    void record_provide(std::size_t provider, std::uint64_t id, std::string name) {
        provides_.push_back(ProvideRecord{provider, id, std::move(name)});
    }

    // Drop the record of one registration; the service itself is erased by
    // the providing component's scope.
    void clear_provide(std::size_t provider, std::uint64_t id) {
        provides_.erase(std::remove_if(provides_.begin(), provides_.end(),
                                       [&](const ProvideRecord& r) {
                                           return r.provider == provider && r.service_id == id;
                                       }),
                        provides_.end());
    }

    void clear_provides_for(std::size_t provider) {
        provides_.erase(std::remove_if(provides_.begin(), provides_.end(),
                                       [&](const ProvideRecord& r) { return r.provider == provider; }),
                        provides_.end());
    }

    std::size_t provider_of(std::uint64_t id) const noexcept {
        for (const auto& r : provides_)
            if (r.service_id == id) return r.provider;
        return npos;
    }

    const std::vector<ProvideRecord>& provides() const noexcept { return provides_; }

    // ---- published services (the C ABI surface, see published.hpp) --------

    PublishedRegistry& published() noexcept { return published_; }
    const PublishedRegistry& published() const noexcept { return published_; }

    // ---- logging ----------------------------------------------------------

    // Where nostos_host_api::log lands. A std::function rather than an
    // interface: the ABI layer calls it on a foreign call stack and must not
    // care who owns the sink. Never let it throw out of the ABI thunk.
    using LogSink = std::function<void(int level, std::string_view msg)>;

    void set_log_sink(LogSink sink) { log_sink_ = sink ? std::move(sink) : default_log_sink(); }
    const LogSink& log_sink() const noexcept { return log_sink_; }

    void log(int level, std::string_view msg) const {
        if (log_sink_) log_sink_(level, msg);
    }

    // "[nostos][warn] msg" on stderr — the host's last-resort sink (a plugin
    // logging during a failed load still has somewhere to go).
    static LogSink default_log_sink() {
        return [](int level, std::string_view msg) {
            static const char* names[] = {"trace", "debug", "info", "warn", "error"};
            const char* name = (level >= 0 && level <= 4) ? names[level] : "?";
            std::fprintf(stderr, "[nostos][%s] %.*s\n", name, static_cast<int>(msg.size()),
                         msg.data());
        };
    }

    // ---- declared Provides (di.hpp) --------------------------------------

    void declare_provides(std::size_t component, std::vector<std::uint64_t> ids) {
        if (declared_provides_.size() <= component) declared_provides_.resize(component + 1);
        declared_provides_[component] = std::move(ids);
    }

    // true when registering id for this component is allowed: a component
    // that declares no Provides (or an empty list) is unconstrained.
    bool provide_is_declared(std::size_t component, std::uint64_t id) const noexcept {
        if (component >= declared_provides_.size()) return true;
        const auto& ids = declared_provides_[component];
        if (ids.empty()) return true;
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    // ---- declared Requires (di.hpp) --------------------------------------
    //
    // The acquire side needs one thing the provide side does not: the *type*.
    // `Requires = Svc<"x", A>` and a call site `service<"x", B>()` are two
    // independent pieces of text, and di.hpp only ever compares declarations
    // with each other. Without a type-aware check here, that mismatch would be
    // invisible in a release build (the Debug-only registry check is the only
    // other guard, and it does not fire in NDEBUG).

    struct DeclaredRequire {
        std::uint64_t service_id;
        const std::type_info* type;
    };

    enum class RequireStatus {
        unconstrained,  // component declares no Requires — anything goes
        matched,        // declared, and with exactly the requested type
        not_listed,     // declares a non-empty list that does not contain this id
        type_conflict,  // declares the id, but under a different C++ type
    };

    struct RequireCheck {
        RequireStatus status = RequireStatus::unconstrained;
        const std::type_info* declared = nullptr;  // set when type_conflict
    };

    void declare_requires(std::size_t component, std::vector<DeclaredRequire> list) {
        if (declared_requires_.size() <= component) declared_requires_.resize(component + 1);
        declared_requires_[component] = std::move(list);
    }

    RequireCheck check_require(std::size_t component, std::uint64_t id,
                               const std::type_info& requested) const noexcept {
        if (component >= declared_requires_.size()) return {RequireStatus::unconstrained, nullptr};
        const auto& list = declared_requires_[component];
        if (list.empty()) return {RequireStatus::unconstrained, nullptr};
        for (const auto& entry : list) {
            if (entry.service_id != id) continue;
            if (entry.type != nullptr && *entry.type == requested)
                return {RequireStatus::matched, entry.type};
            return {RequireStatus::type_conflict, entry.type};
        }
        return {RequireStatus::not_listed, nullptr};
    }

private:
    ServiceRegistry services_;
    EventHub events_;
    PublishedRegistry published_;
    Scope root_;
    InlineExecutor default_executor_;
    Executor* executor_ = &default_executor_;
    std::vector<DependencyEdge> edges_;
    std::vector<ProvideRecord> provides_;
    std::vector<std::vector<std::uint64_t>> declared_provides_;
    std::vector<std::vector<DeclaredRequire>> declared_requires_;
    LogSink log_sink_ = default_log_sink();
};

class Context {
public:
    Context(HostCore& host, std::size_t component_index, Scope& scope) noexcept
        : host_(&host), index_(component_index), scope_(&scope) {}

    // The scope this component's side effects belong to. Guard anything
    // created here (guards, defers, child scopes) and it unwinds with the
    // component — order guaranteed, no manual cleanup.
    Scope& scope() const noexcept { return *scope_; }
    std::size_t component_index() const noexcept { return index_; }

    // ---- services --------------------------------------------------------

    // Provide a service owned by this component's scope: registered now,
    // unregistered automatically when the scope unwinds. The unregistration is
    // registered *here*, so its place in the LIFO order is fixed at this moment:
    // cleanup a component defers after this call runs before the service
    // disappears — the usual shape (provide first, defer later). A defer
    // registered *before* the provide runs after the unregistration, i.e. the
    // service is already gone by then. A component that declares Provides may
    // only register what it declared (see di.hpp).
    //
    // Lifetime: the returned reference is a *bare* T&, not a keep-alive handle.
    // It is valid only while this component stays active; consumers that must
    // survive revocation hold the ServiceHandle from service()/require(), which
    // keeps the object alive. A bare T& does not: measured, after
    // host.revoke<"id">() the old reference reads freed memory (0xDDDDDDDD —
    // MSVC's debug-heap fill — and nothing can diagnose it).
    template <fixed_string Name, typename T, typename... Args>
    T& provide(Args&&... args) {
        constexpr std::uint64_t id = svc_id<Name>;
        if (!host_->provide_is_declared(index_, id))
            throw undeclared_provide(Name.view(), index_);
        T& ref = host_->services().template provide<Name, T>(std::forward<Args>(args)...);
        const std::size_t provider = index_;
        host_->record_provide(provider, id, std::string(Name.view()));
        scope_->defer([host = host_, provider, id] {
            host->services().erase(id);
            host->clear_provide(provider, id);
        });
        return ref;
    }

    // Acquire a service. Registers a dependency edge (取用即依赖) and returns
    // a shared handle that stays safe even if the service is revoked later.
    // Throws missing_service when nothing provides Name.
    //
    // The declared Requires list is checked *before* the lookup, so a call site
    // that drifted from the declaration is reported as exactly that (see
    // undeclared_require / declared_type_mismatch) rather than as a missing or
    // wrongly-typed service. Components that declare no Requires are
    // unconstrained, like provide_is_declared.
    template <fixed_string Name, typename T>
    ServiceHandle<T> service() const {
        constexpr std::uint64_t id = svc_id<Name>;
        const auto check = host_->check_require(index_, id, typeid(T));
        if (check.status == HostCore::RequireStatus::not_listed)
            throw undeclared_require(Name.view(), index_);
        if (check.status == HostCore::RequireStatus::type_conflict)
            throw declared_type_mismatch(Name.view(), index_, *check.declared, typeid(T));
        auto ptr = host_->services().template find<Name, T>();
        if (!ptr) throw missing_service(Name.view(), id);
        host_->record_dependency(index_, id, std::string(Name.view()));
        return ServiceHandle<T>(std::move(ptr));
    }

    // Borrowed access — same dependency recording, reference semantics.
    template <fixed_string Name, typename T>
    T& require() const {
        return *this->template service<Name, T>();
    }

    // ---- published services (C interface tables, see published.hpp) -------

    // Publish a C interface table under Name for this component's lifetime:
    // resolvable through nostos_host_api::service from any ABI domain, and
    // revoked automatically when the component's scope unwinds. The table must
    // be stamped with abi::init_table and must outlive the scope (a static or
    // member, never a temporary). Same declared-Provides rule as provide().
    //
    // The registry does NOT own the table (published.hpp), and it cannot check
    // storage duration — nothing in the type system carries it. If the table has
    // static storage duration, prefer publish_static() below: it makes the
    // compiler enforce exactly that rule. This form remains for member tables.
    template <fixed_string Name, abi::Table T>
    const T& publish(const T& table) {
        constexpr std::uint64_t id = svc_id<Name>;
        if (!host_->provide_is_declared(index_, id))
            throw undeclared_provide(Name.view(), index_);
        host_->published().add(id, &table, Name.view());
        const std::size_t provider = index_;
        host_->record_provide(provider, id, std::string(Name.view()));
        scope_->defer([host = host_, provider, id] {
            host->published().remove(id);
            host->clear_provide(provider, id);
        });
        return table;
    }

    // The compile-time-checked form of publish() for a table with **static
    // storage duration**. A reference non-type template argument may only name an
    // object with static storage, so publishing a local — the mistake nothing
    // else can catch, because storage duration is not part of a type — fails to
    // compile instead of leaving the registry with a pointer into a dead stack
    // frame (P3-4 in docs/known-issues.md).
    //
    // Use publish() for a table that is a member of the component: that lifetime
    // is the component's, which the scope already governs, and a reference NTTP
    // cannot name a non-static member. Passing a local (or a member of a local)
    // here reports "no matching overload" — an argument that is not a valid
    // template argument has no place to hang a friendlier message.
    template <fixed_string Name, const auto& Table>
        requires abi::Table<std::remove_cvref_t<decltype(Table)>>
    void publish_static() {
        (void)publish<Name>(Table);
    }

    // Resolve a published table from the same ABI domain (host code, another
    // component). Version-gated: an incompatible revision is reported as
    // plugin_error rather than as a missing service, because the two failures
    // have completely different fixes.
    template <fixed_string Name, abi::Table T>
    const T* interface(std::uint32_t min_version = 1) const {
        constexpr std::uint64_t id = svc_id<Name>;
        const T* table = host_->published().template find_as<T>(id, min_version);
        if (table == nullptr) {
            if (host_->published().contains(id))
                throw plugin_error("nostos: published service '" + std::string(Name.view()) +
                                   "' exists but its interface revision is incompatible with " +
                                   "this consumer");
            throw missing_service(Name.view(), id);
        }
        host_->record_dependency(index_, id, std::string(Name.view()));
        return table;
    }

    // ---- events ----------------------------------------------------------

    template <EventType E, typename F>
        requires std::invocable<std::decay_t<F>&, const E&>
    [[nodiscard]] Subscription on(F&& fn) {
        return host_->events().on<E>(std::forward<F>(fn));
    }

    // Subscribe and hand the token straight to this component's scope, so the
    // subscription is torn down with everything else the component did. Most
    // components want this form: on() alone returns a token that must be kept,
    // and a dropped token unsubscribes at the end of the statement — a silent
    // no-op rather than an error (see tests/test_event.cpp and P2-7 in
    // docs/known-issues.md). on() is the form for a subscription that must
    // outlive a particular scope or be cancelled early.
    template <EventType E, typename F>
        requires std::invocable<std::decay_t<F>&, const E&>
    void on_scoped(F&& fn) {
        scope_->own(host_->events().on<E>(std::forward<F>(fn)));
    }

    template <EventType E>
    void emit(const E& e) {
        host_->events().emit(e);
    }

    // ---- scheduling seam --------------------------------------------------

    // The executor this host runs on. A component that wants to defer work
    // ("finish this after the current activation/dispatch completes") posts to
    // it instead of doing the work inline; the host decides what that means
    // (Host::set_executor, see executor.hpp). The core itself never posts — it
    // runs everything synchronously on the caller's thread.
    Executor& executor() const noexcept { return host_->executor(); }

    // ---- escape hatches (host-side wiring, tests) -------------------------

    ServiceRegistry& services() const noexcept { return host_->services(); }
    EventHub& events() const noexcept { return host_->events(); }
    HostCore& host() const noexcept { return *host_; }

private:
    HostCore* host_;
    std::size_t index_;
    Scope* scope_;
};

}  // namespace nostos
