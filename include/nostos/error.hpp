#pragma once
// nostos L0 — the error vocabulary.
//
// Every error the library throws derives from nostos_error, so a host can
// catch the whole family with one clause while still being able to name the
// specific failure. Messages are built at throw time and always name the
// offending entity (service, component, event, plugin path) — this library's
// diagnostics rule applies to runtime errors exactly as it does to
// compile-time ones (see di.hpp).
//
// Each error ALSO carries its identifying data as members with accessors, so a
// host can act on a failure programmatically instead of parsing what(). That
// matters for planned work such as Phase 4's PENDING auto-reactivation, where
// the host has to know *which* service id it is waiting for in order to
// register a waiter. Humans read what(); programs read the accessors.
//
// NOTE: nothing in this header may throw out of a dynamic-plugin boundary.
// The C ABI reports failures through nostos_status instead (nostos_abi.h,
// discipline 2); these types are for host-side and same-ABI-domain code.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>

namespace nostos {

class nostos_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

inline std::string hex64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

// Every "named service + id" message has the same shape:
//   <prefix><name>' (<id>)<suffix>
// Keeping it in one place means the text cannot drift between the error types
// (and the tests that assert on it keep working).
inline std::string quoted_id_message(std::string_view prefix, std::string_view name,
                                     std::uint64_t id, std::string_view suffix) {
    return std::string(prefix) + std::string(name) + "' (" + hex64(id) + ")" + std::string(suffix);
}

}  // namespace detail

class missing_service : public nostos_error {
public:
    missing_service(std::string_view name, std::uint64_t id)
        : nostos_error(detail::quoted_id_message("nostos: no service provides '", name, id, "")),
          name_(name),
          id_(id) {}

    // What the host needs to register a waiter (Phase 4 PENDING) or to route a
    // failure by service: the same id the registry is keyed by.
    std::string_view service_name() const noexcept { return name_; }
    std::uint64_t service_id() const noexcept { return id_; }

private:
    std::string name_;  // owned: the caller may hand us a view of a temporary
    std::uint64_t id_ = 0;
};

class duplicate_service : public nostos_error {
public:
    duplicate_service(std::string_view name, std::uint64_t id)
        : nostos_error(detail::quoted_id_message("nostos: service '", name, id,
                                                 " is already provided")),
          name_(name),
          id_(id) {}

    std::string_view service_name() const noexcept { return name_; }
    std::uint64_t service_id() const noexcept { return id_; }

private:
    std::string name_;
    std::uint64_t id_ = 0;
};

class type_mismatch : public nostos_error {
public:
    type_mismatch(std::string_view name, const std::type_info& requested,
                  const std::type_info& registered)
        : nostos_error("nostos: service '" + std::string(name) + "' is registered as '" +
                       registered.name() + "' but requested as '" + requested.name() + "'"),
          name_(name),
          requested_(requested.name()),
          registered_(registered.name()) {}

    std::string_view service_name() const noexcept { return name_; }

    // NOTE: these are std::type_info::name() spellings — implementation
    // defined (MSVC prints "struct A", GCC prints a mangled name). They are
    // meaningful only inside one ABI domain, exactly like the check that threw.
    std::string_view requested_type() const noexcept { return requested_; }
    std::string_view registered_type() const noexcept { return registered_; }

private:
    std::string name_;
    std::string_view requested_;   // type_info::name() has static lifetime
    std::string_view registered_;
};

// A component that declares Provides (see di.hpp) registered a service its
// declaration does not list: the declared contract and the implementation
// have drifted apart. The compile-time check cannot see this, so it is
// enforced at registration time.
class undeclared_provide : public nostos_error {
public:
    undeclared_provide(std::string_view name, std::size_t component)
        : nostos_error("nostos: component #" + std::to_string(component) + " provides '" +
                       std::string(name) + "' but its Provides declaration does not list it"),
          name_(name),
          component_(component) {}

    std::string_view service_name() const noexcept { return name_; }
    std::size_t component_index() const noexcept { return component_; }

private:
    std::string name_;
    std::size_t component_ = 0;
};

// A component acquired a service whose id its Requires declaration does not
// list at all. Mirror image of undeclared_provide: the declared contract and
// the implementation have drifted apart, and the compile-time check cannot see
// a call site.
class undeclared_require : public nostos_error {
public:
    undeclared_require(std::string_view name, std::size_t component)
        : nostos_error("nostos: component #" + std::to_string(component) + " requires '" +
                       std::string(name) + "' but its Requires declaration does not list it"),
          name_(name),
          component_(component) {}

    std::string_view service_name() const noexcept { return name_; }
    std::size_t component_index() const noexcept { return component_; }

private:
    std::string name_;
    std::size_t component_ = 0;
};

// The component DID declare this id, but under a different C++ type than the
// call site asks for — `Requires = Svc<"x", A>` while calling
// `service<"x", B>()`. di.hpp checks declarations against each other; nothing
// but this check can compare a declaration with the call site. Without it the
// mismatch would silently reinterpret memory in a release build.
class declared_type_mismatch : public nostos_error {
public:
    declared_type_mismatch(std::string_view name, std::size_t component,
                           const std::type_info& declared, const std::type_info& requested)
        : nostos_error("nostos: component #" + std::to_string(component) + " requires '" +
                       std::string(name) + "' as '" + requested.name() +
                       "' but its Requires declaration lists '" + declared.name() + "'"),
          name_(name),
          component_(component),
          declared_(declared.name()),
          requested_(requested.name()) {}

    std::string_view service_name() const noexcept { return name_; }
    std::size_t component_index() const noexcept { return component_; }

    // type_info::name() spellings (implementation defined), meaningful only
    // inside one ABI domain — same caveat as type_mismatch.
    std::string_view declared_type() const noexcept { return declared_; }
    std::string_view requested_type() const noexcept { return requested_; }

private:
    std::string name_;
    std::size_t component_ = 0;
    std::string_view declared_;
    std::string_view requested_;
};

// A published table failed validation at registration time. The registry
// refuses unstamped tables outright, so this is a hard contract violation at the
// boundary rather than a recoverable state — but it still names what is wrong
// and carries the offending value, so a host can report it without string
// matching.
class bad_published_table : public nostos_error {
public:
    enum class Reason {
        null_table,   // no table pointer at all
        struct_size,  // struct_size does not even cover the TableHeader
        abi_version,  // abi_version == 0: the table was never stamped
    };

    bad_published_table(std::string_view name, Reason reason, std::uint32_t value = 0)
        : nostos_error(message(name, reason, value)),
          name_(name),
          reason_(reason),
          value_(value) {}

    std::string_view service_name() const noexcept { return name_; }
    Reason reason() const noexcept { return reason_; }
    // struct_size for Reason::struct_size, abi_version for Reason::abi_version,
    // 0 for null_table.
    std::uint32_t offending_value() const noexcept { return value_; }

private:
    static std::string message(std::string_view name, Reason reason, std::uint32_t value) {
        switch (reason) {
            case Reason::null_table:
                return "nostos: published service '" + std::string(name) +
                       "' has no interface table";
            case Reason::struct_size:
                return "nostos: published service '" + std::string(name) +
                       "' has an unstamped table (struct_size=" + std::to_string(value) + ")";
            case Reason::abi_version:
                return "nostos: published service '" + std::string(name) +
                       "' declares abi_version 0";
        }
        return "nostos: published service '" + std::string(name) + "' is invalid";
    }

    std::string name_;
    Reason reason_ = Reason::null_table;
    std::uint32_t value_ = 0;
};

// Calling a Host entry point out of order (start() twice, activate() before
// start(), re-activating a live component). This is misuse of the host's own
// API, not a component failure — which is why it does not travel through the
// component's scope rollback. The component index is carried when the call
// concerned one.
class host_state_error : public nostos_error {
public:
    enum class Reason {
        already_started,  // start() on a fully activated host
        not_started,      // activate(index) with the host not started
        already_active,   // activate(index) on a live component
    };

    static constexpr std::size_t no_component = static_cast<std::size_t>(-1);

    explicit host_state_error(Reason reason, std::size_t component = no_component)
        : nostos_error(message(reason)), reason_(reason), component_(component) {}

    Reason reason() const noexcept { return reason_; }
    std::size_t component_index() const noexcept { return component_; }

private:
    static std::string message(Reason reason) {
        switch (reason) {
            case Reason::already_started:
                return "nostos: host is already started";
            case Reason::not_started:
                return "nostos: activate(index) needs a started host";
            case Reason::already_active:
                return "nostos: component is already active";
        }
        return "nostos: host is in the wrong state";
    }

    Reason reason_ = Reason::not_started;
    std::size_t component_ = no_component;
};

// Two different payload types share one event name. The raw channel keys on
// the name hash, so dispatching a plugin's payload through the wrong bus
// would reinterpret C++ types; refuse instead of corrupting memory.
class event_name_collision : public nostos_error {
public:
    event_name_collision(std::string_view name, std::uint64_t id)
        : nostos_error(detail::quoted_id_message("nostos: event name '", name, id,
                                                 " is used by two different payload types")),
          name_(name),
          id_(id) {}

    std::string_view event_name() const noexcept { return name_; }
    // Not named event_id(): that would shadow nostos::event_id<E> inside this
    // class (and inside any class deriving from it).
    std::uint64_t event_id_value() const noexcept { return id_; }

private:
    std::string name_;
    std::uint64_t id_ = 0;
};

// Loading, validating or driving a dynamic plugin failed. The message carries
// the OS error text or the ABI mismatch, never a bare code.
class plugin_error : public nostos_error {
public:
    using nostos_error::nostos_error;
};

}  // namespace nostos
