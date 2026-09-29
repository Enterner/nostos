#pragma once
// nostos L1 — compile-time dependency declarations and the static check.
//
// Runtime acquisition (取用即依赖, see context.hpp) already guarantees that a
// service exists before it is used: acquiring one is what records the edge,
// and a missing service throws and rolls the activation back. What runtime
// cannot do is tell you *before* the process starts that a Host was assembled
// from components whose declared dependencies can never be satisfied. That is
// this header's job.
//
// A component MAY declare both sides of its dependency contract:
//
//     struct Config {};
//     struct Frobber {};
//
//     struct MyPlugin {
//         using Requires = std::tuple<Svc<"nostos.config", Config>>;
//         using Provides = std::tuple<Svc<"my.frobber",   Frobber>>;
//         void activate(nostos::Context& ctx);
//     };
//
//     nostos::Host<MyPlugin, OtherPlugin> host;   // checked here, at compile time
//
// Host<Cs...> then enforces three rules over the whole component set:
//
//   1. every Requires is covered by the union of all Provides
//      (identity = the compile-time name hash, never typeid);
//   2. a service provided and required under *different* C++ types is an
//      error, not a debug-only surprise at first acquisition;
//   3. every provided svc_id is unique, so a name collision or a copy-pasted
//      name is caught at compile time instead of as a runtime
//      duplicate_service.
//
// Declaration is opt-in and per component: declare neither Requires nor
// Provides and the component behaves exactly as before. The rules compare
// declared Provides only, so in a Host that mixes declaring and non-declaring
// components the contract must be declared on *both* sides (the provider's
// Provides and the consumer's Requires) — otherwise the consumer's Requires
// looks unsatisfied.
//
// Diagnostics: C++20 cannot compute a static_assert message, so the failing
// instantiation *is* the message — the compiler prints the offending
// component and service inside the type name:
//
//     error: static assertion failed: nostos: a component's Requires is not
//            covered by this Host's Provides — ...
//     note:  while instantiating 'unsatisfied_requirement_error<
//              MyPlugin, Svc<"nostos.config", Config>, void>'
//
// One rendering caveat, measured: MSVC prints the name of a `Svc` as its
// character codes rather than the literal —
//     nostos::Svc<nostos::fixed_string<6>{char120,46,115,118,99,0}, int>   // "x.svc"
// — while GCC/Clang print the literal. The component is always named plainly, so
// the diagnostic stays usable either way; tests/compile_fail/expect_compile_failure.cmake
// asserts on both renderings for exactly this reason.
//
// The declaration shape is validated too: Requires/Provides must be
// std::tuple<Svc<Name, T>...>.

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "nostos/svc_id.hpp"

namespace nostos {

// One service identity: the compile-time name that keys the registry, plus the
// C++ type behind it (meaningful only inside a single ABI domain).
template <fixed_string Name, typename T>
struct Svc {
    using type = T;
    static constexpr fixed_string name = Name;
    static constexpr std::uint64_t id = svc_id<Name>;
};

namespace detail {

template <typename...>
inline constexpr bool always_false_v = false;

template <typename T>
struct is_svc : std::false_type {};
template <fixed_string N, typename T>
struct is_svc<Svc<N, T>> : std::true_type {};

// std::tuple of Svc<...> — the only accepted declaration shape.
template <typename T>
struct is_svc_list : std::false_type {};
template <typename... S>
struct is_svc_list<std::tuple<S...>> : std::bool_constant<(is_svc<S>::value && ...)> {};

// Requires/Provides are optional: an undeclared side is the empty list.
template <typename C, typename = void>
struct requires_of : std::type_identity<std::tuple<>> {};
template <typename C>
struct requires_of<C, std::void_t<typename C::Requires>>
    : std::type_identity<typename C::Requires> {};

template <typename C, typename = void>
struct provides_of : std::type_identity<std::tuple<>> {};
template <typename C>
struct provides_of<C, std::void_t<typename C::Provides>>
    : std::type_identity<typename C::Provides> {};

// ---- set algebra over Svc lists ---------------------------------------

// First element of Ss... whose id equals Id; void when there is none.
template <std::uint64_t Id, typename... Ss>
struct first_with_id {
    using type = void;
    static constexpr bool found = false;
};
template <std::uint64_t Id, typename S0, typename... Rest>
struct first_with_id<Id, S0, Rest...> {
    static constexpr bool found = (S0::id == Id) || first_with_id<Id, Rest...>::found;
    using type = std::conditional_t<S0::id == Id, S0, typename first_with_id<Id, Rest...>::type>;
};

template <std::uint64_t Id, typename List>
struct first_in_list;
template <std::uint64_t Id, typename... Ss>
struct first_in_list<Id, std::tuple<Ss...>> : first_with_id<Id, Ss...> {};

// true when the provided service P exists and carries exactly R's type.
template <typename P, typename R, bool = std::is_void_v<P>>
struct types_agree : std::false_type {};
template <typename P, typename R>
struct types_agree<P, R, false> : std::bool_constant<std::is_same_v<typename P::type, typename R::type>> {};

// Concatenation of every component's Provides list.
template <typename... Lists>
struct flatten;
template <>
struct flatten<> {
    using type = std::tuple<>;
};
template <typename... S, typename... Rest>
struct flatten<std::tuple<S...>, Rest...> {
    using type = decltype(std::tuple_cat(
        std::declval<std::tuple<S...>>(), std::declval<typename flatten<Rest...>::type>()));
};

// ---- findings ----------------------------------------------------------

struct requirement_ok {
    using type = requirement_ok;
    static constexpr bool found = false;
    using plugin = void;
    using service = void;
    using provided = void;
};

// A Requires entry that no Provides covers, or — when provided != void — one
// that is covered under a different C++ type.
template <typename Plugin, typename Required, typename Provided>
struct unsatisfied_requirement {
    using type = unsatisfied_requirement;
    static constexpr bool found = true;
    static constexpr bool type_conflict = !std::is_void_v<Provided>;
    using plugin = Plugin;
    using service = Required;
    using provided = Provided;
};

struct provision_ok {
    using type = provision_ok;
    static constexpr bool found = false;
    using first = void;
    using second = void;
};

template <typename First, typename Second>
struct duplicate_provision {
    using type = duplicate_provision;
    static constexpr bool found = true;
    using first = First;
    using second = Second;
};

// ---- the scans ---------------------------------------------------------

// One component's Requires against the host-wide Provides.
template <typename Provided, typename Plugin, typename... Rs>
struct scan_requires;
template <typename Provided, typename Plugin>
struct scan_requires<Provided, Plugin> : requirement_ok {};
template <typename Provided, typename Plugin, typename R0, typename... Rs>
struct scan_requires<Provided, Plugin, R0, Rs...> {
    using look = typename first_in_list<R0::id, Provided>::type;
    using tail = typename scan_requires<Provided, Plugin, Rs...>::type;
    static constexpr bool missing = std::is_void_v<look>;
    static constexpr bool mismatched = !missing && !types_agree<look, R0>::value;
    using type = std::conditional_t<
        missing, unsatisfied_requirement<Plugin, R0, void>,
        std::conditional_t<mismatched, unsatisfied_requirement<Plugin, R0, look>, tail>>;
    static constexpr bool found = type::found;
    using plugin = typename type::plugin;
    using service = typename type::service;
    using provided = typename type::provided;
};

template <typename Provided, typename Plugin, typename List>
struct scan_one;
template <typename Provided, typename Plugin, typename... Rs>
struct scan_one<Provided, Plugin, std::tuple<Rs...>> : scan_requires<Provided, Plugin, Rs...> {};

// First unsatisfied requirement across all components. Declaration order
// decides which offender is reported first.
template <typename Provided, typename... Cs>
struct scan_all {
    using type = requirement_ok;
    static constexpr bool found = false;
    using plugin = void;
    using service = void;
    using provided = void;
};
template <typename Provided, typename C0, typename... Cs>
struct scan_all<Provided, C0, Cs...> {
    using here = typename scan_one<Provided, C0, typename requires_of<C0>::type>::type;
    using tail = typename scan_all<Provided, Cs...>::type;
    using type = std::conditional_t<here::found, here, tail>;
    static constexpr bool found = type::found;
    using plugin = typename type::plugin;
    using service = typename type::service;
    using provided = typename type::provided;
};

// First duplicated provided id.
template <typename... Ss>
struct dup_scan : provision_ok {};
template <typename S0, typename... Rest>
struct dup_scan<S0, Rest...> {
    static constexpr bool here = ((S0::id == Rest::id) || ...);
    using tail = dup_scan<Rest...>;
    using type = std::conditional_t<here, duplicate_provision<S0, typename first_with_id<S0::id, Rest...>::type>,
                                    typename tail::type>;
    static constexpr bool found = type::found;
    using first = typename type::first;
    using second = typename type::second;
};

template <typename List>
struct dup_scan_of;
template <typename... Ss>
struct dup_scan_of<std::tuple<Ss...>> : dup_scan<Ss...> {};

// ---- failing instantiations (the actual diagnostics) -------------------

// Instantiated only when the matching check fails. The message is fixed
// (C++20 has no computed static_assert strings); the *type* carries the names
// the compiler prints, so the offending component and service show up in the
// instantiation note. The parameters are spelled out one by one rather than
// passing the finding type through, because compilers print an alias (e.g.
// Host<...>::check_dependencies::unsatisfied) instead of expanding it.
template <typename Plugin, typename Service, typename Provided>
struct unsatisfied_requirement_error {
    static_assert(always_false_v<Plugin, Service, Provided>,
                  "nostos: a component's Requires is not covered by this Host's Provides — the "
                  "component and the missing/conflicting service are named in the instantiation "
                  "of nostos::detail::unsatisfied_requirement_error below (Provided = void means "
                  "'nothing provides it', otherwise the service exists under a different type)");
};

template <typename First, typename Second>
struct duplicate_provision_error {
    static_assert(always_false_v<First, Second>,
                  "nostos: two components Provide the same service id — the duplicated service "
                  "is named in the instantiation of nostos::detail::duplicate_provision_error "
                  "below (svc_id collision or a copy-pasted name)");
};

}  // namespace detail

// ---- the public analysis surface --------------------------------------
//
// Pure compile-time analysis, exposed so tests and tools can inspect a Host's
// dependency contract without triggering the errors.

namespace di {

template <typename C>
using requires_t = typename detail::requires_of<C>::type;
template <typename C>
using provides_t = typename detail::provides_of<C>::type;

template <typename C>
inline constexpr bool declares_requires_v = requires { typename C::Requires; };
template <typename C>
inline constexpr bool declares_provides_v = requires { typename C::Provides; };

// Requires/Provides must be std::tuple<Svc<Name, T>...> when present.
template <typename C>
inline constexpr bool well_formed_v =
    (!declares_requires_v<C> || detail::is_svc_list<requires_t<C>>::value) &&
    (!declares_provides_v<C> || detail::is_svc_list<provides_t<C>>::value);

// Union of every component's Provides.
template <typename... Cs>
using provided_t = typename detail::flatten<provides_t<Cs>...>::type;

// Analysis results, each with ::found plus the offending types.
template <typename... Cs>
using unsatisfied_t = typename detail::scan_all<provided_t<Cs...>, Cs...>::type;
template <typename... Cs>
using duplicate_t = typename detail::dup_scan_of<provided_t<Cs...>>::type;

template <typename... Cs>
inline constexpr bool consistent_v = !unsatisfied_t<Cs...>::found && !duplicate_t<Cs...>::found;

// A component's declared ids as a value: HostCore uses it to keep
// Context::provide honest against the declaration.
template <typename List>
struct id_list;
template <typename... S>
struct id_list<std::tuple<S...>> {
    static constexpr std::array<std::uint64_t, sizeof...(S)> value{S::id...};
    static constexpr std::size_t size = sizeof...(S);
};

}  // namespace di

}  // namespace nostos
