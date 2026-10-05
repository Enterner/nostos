#pragma once
// nostos L1 —— 静态 component 协议。
//
// component 是一个带 activate(Context&) 的可默认构造类。
// * activate() 经由 ctx 完成全部接线，并把副作用收进 ctx.scope()。它可以
//   抛异常：激活是事务性的，它已经做的一切都会被自动回滚。
// * onStop() 可选。它在 component 的 scope unwind 之前运行，用于需要按顺序
//   进行的清理（flush → disconnect → release）。必须 noexcept——回滚不许
//   失败。

#include <concepts>
#include <type_traits>

#include "nostos/context.hpp"

namespace nostos {

template <typename C>
concept Component = std::is_class_v<C> && std::default_initializable<C> &&
                    requires(C& c, Context& ctx) { c.activate(ctx); };

// component 是否声明了 onStop()（不要求 noexcept）。
template <typename C>
concept DeclaresOnStop = requires(C& c, Context& ctx) {
    c.activate(ctx);
    c.onStop();
};

}  // namespace nostos
