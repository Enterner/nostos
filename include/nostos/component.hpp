#pragma once
// nostos L1 — the static component protocol.
//
// A component is a default-constructible class with activate(Context&).
// * activate() wires everything through ctx and owns its side effects in
//   ctx.scope(). It MAY throw: activation is transactional, and everything
//   it already did is rolled back automatically.
// * onStop() is optional. It runs before the component's scope unwinds, for
//   cleanups that need ordered steps (flush → disconnect → release). It must
//   be noexcept — rollback must not fail.

#include <concepts>
#include <type_traits>

#include "nostos/context.hpp"

namespace nostos {

template <typename C>
concept Component = std::is_class_v<C> && std::default_initializable<C> &&
                    requires(C& c, Context& ctx) { c.activate(ctx); };

// true when the component declares onStop() at all (no noexcept requirement).
template <typename C>
concept DeclaresOnStop = requires(C& c, Context& ctx) {
    c.activate(ctx);
    c.onStop();
};

}  // namespace nostos
