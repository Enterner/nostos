#pragma once
// nostos L1 — 编译期依赖声明与静态检查。
//
// 运行期取用（取用即依赖，见 context.hpp）已经保证 service 在被使用之前
// 必然存在：取用本身就是记录依赖边的动作，service 缺失时抛异常并回滚
// 本次激活。运行期做不到的，是在进程启动*之前*就告诉你：某个 Host 由
// 一批组件拼装而成，而它们声明的依赖永远无法被满足。这正是本头文件的
// 职责。
//
// 组件可以（MAY）声明其依赖契约的两端：
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
//     nostos::Host<MyPlugin, OtherPlugin> host;   // 在此处做编译期检查
//
// Host<Cs...> 随即对整个组件集合强制三条规则：
//
//   1. 每个 Requires 都被所有 Provides 的并集覆盖
//      （同一性 = 编译期名字哈希，绝不使用 typeid）；
//   2. 以*不同* C++ 类型提供与取用同一个 service 是错误，而不是只在
//      Debug 下、首次取用时才爆出的意外；
//   3. 每个 svc_id 的提供必须唯一，名字撞车或复制粘贴来的名字在编译期
//      就被抓住，而不是等到运行期的 duplicate_service。
//
// 声明是可选（opt-in）且按组件生效的：既不声明 Requires 也不声明
// Provides 的组件，行为与未声明时完全一致。规则只比较已声明的
// Provides，因此在声明与不声明组件混用的 Host 中，契约必须在*两端*
// 都声明（提供方的 Provides 与消费方的 Requires）— 否则消费方的
// Requires 会显得未被满足。
//
// 诊断：C++20 无法计算 static_assert 的消息文本，因此失败的实例化
// *本身就是*消息 — 编译器会把出问题的组件与 service 打印在类型名里：
//
//     error: static assertion failed: nostos: a component's Requires is not
//            covered by this Host's Provides — ...
//     note:  while instantiating 'unsatisfied_requirement_error<
//              MyPlugin, Svc<"nostos.config", Config>, void>'
//
// NOTE: 渲染上的一个坑。踩过: 实测 MSVC 把 `Svc` 的名字打印成字符码
// 而非字面量 —
//     nostos::Svc<nostos::fixed_string<6>{char120,46,115,118,99,0}, int>   // "x.svc"
// — 而 GCC/Clang 打印字面量。组件名总是以明文出现，因此无论哪种渲染
// 诊断都可用；tests/compile_fail/expect_compile_failure.cmake 正是为此
// 对两种渲染都做了断言。
//
// 声明的形式本身也会被校验：Requires/Provides 必须是
// std::tuple<Svc<Name, T>...>。

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "nostos/svc_id.hpp"

namespace nostos {

// 一个 service 的身份：作为 registry 键的编译期名字，加上其背后的
// C++ 类型（仅在单个 ABI 域内有意义）。
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

// std::tuple of Svc<...> — 唯一被接受的声明形式。
template <typename T>
struct is_svc_list : std::false_type {};
template <typename... S>
struct is_svc_list<std::tuple<S...>> : std::bool_constant<(is_svc<S>::value && ...)> {};

// Requires/Provides 是可选的：未声明的一侧视为空列表。
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

// ---- Svc 列表上的集合运算 ---------------------------------------

// Ss... 中第一个 id 等于 Id 的元素；不存在则为 void。
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

// 当被提供的 service P 存在、且类型与 R 完全一致时为 true。
template <typename P, typename R, bool = std::is_void_v<P>>
struct types_agree : std::false_type {};
template <typename P, typename R>
struct types_agree<P, R, false> : std::bool_constant<std::is_same_v<typename P::type, typename R::type>> {};

// 所有组件 Provides 列表的拼接。
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

// ---- 检查结果 ----------------------------------------------------------

struct requirement_ok {
    using type = requirement_ok;
    static constexpr bool found = false;
    using plugin = void;
    using service = void;
    using provided = void;
};

// 未被任何 Provides 覆盖的 Requires 条目；或 — 当 provided != void 时 —
// 被以不同 C++ 类型覆盖的条目。
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

// ---- 扫描 ---------------------------------------------------------

// 用全宿主范围的 Provides 检查单个组件的 Requires。
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

// 全部组件中第一个未被满足的 Requires。声明顺序决定先报告哪个违规者。
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

// 第一个被重复提供的 id。
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

// ---- 触发失败的实例化（真正的诊断输出） -------------------

// 仅在对应检查失败时才会被实例化。消息文本是固定的（C++20 没有可计算的
// static_assert 字符串）；承载编译器所打印名字的是*类型*，因而出问题的
// 组件与 service 会出现在实例化 note 中。模板参数逐个写出而非整体传递
// finding 类型，因为编译器只打印别名
// （如 Host<...>::check_dependencies::unsatisfied），不会将其展开。
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

// ---- 公开的分析接口 --------------------------------------
//
// 纯编译期分析，供测试与工具在不触发那些错误的前提下检查 Host 的依赖
// 契约。

namespace di {

template <typename C>
using requires_t = typename detail::requires_of<C>::type;
template <typename C>
using provides_t = typename detail::provides_of<C>::type;

template <typename C>
inline constexpr bool declares_requires_v = requires { typename C::Requires; };
template <typename C>
inline constexpr bool declares_provides_v = requires { typename C::Provides; };

// 若声明了 Requires/Provides，则必须是 std::tuple<Svc<Name, T>...>。
template <typename C>
inline constexpr bool well_formed_v =
    (!declares_requires_v<C> || detail::is_svc_list<requires_t<C>>::value) &&
    (!declares_provides_v<C> || detail::is_svc_list<provides_t<C>>::value);

// 所有组件 Provides 的并集。
template <typename... Cs>
using provided_t = typename detail::flatten<provides_t<Cs>...>::type;

// 分析结果，各带 ::found 与违规类型。
template <typename... Cs>
using unsatisfied_t = typename detail::scan_all<provided_t<Cs...>, Cs...>::type;
template <typename... Cs>
using duplicate_t = typename detail::dup_scan_of<provided_t<Cs...>>::type;

template <typename... Cs>
inline constexpr bool consistent_v = !unsatisfied_t<Cs...>::found && !duplicate_t<Cs...>::found;

// 以值的形式给出某组件声明的 id 列表：HostCore 用它约束
// Context::provide 不得超出声明。
template <typename List>
struct id_list;
template <typename... S>
struct id_list<std::tuple<S...>> {
    static constexpr std::array<std::uint64_t, sizeof...(S)> value{S::id...};
    static constexpr std::size_t size = sizeof...(S);
};

}  // namespace di

}  // namespace nostos
