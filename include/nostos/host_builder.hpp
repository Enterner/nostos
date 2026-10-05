#pragma once
// nostos L1 — Host：激活、事务式回滚、逆序 teardown、动态（部分）卸载。
//
// Host<Cs...> 按声明顺序激活组件。既然 service 必须先被提供才能被取用
// （否则 service() 抛异常），声明顺序就*是*拓扑序 — 无需运行期解析器，
// 也无需环检测：环意味着「先取用、后提供」，那只会在运行期失败并回滚。
//
// start() 是一个事务：任一 activate() 抛出异常时，所有已激活的组件 —
// 以及失败组件自身的部分状态 — 都会被回滚。shutdown() 按完全相反的
// 顺序执行：onStop() → scope unwind（LIFO，同时注销 service）→ 组件
// 析构。
//
// 静态契约分两半：
//
//   * 编译期（di.hpp）：已声明的 Requires 必须被已声明 Provides 的并集
//     覆盖且 C++ 类型一致，并且每个 svc_id 的提供必须唯一。
//   * 运行期：宿主运行期间 service 可以被撤销。unload(i) 与 revoke(id)
//     先回滚所有依赖方（依赖方在前，按逆激活序），*之后*才回滚提供者
//     — 与整体 shutdown 相同的全局逆序，只是限定在受影响的闭包内。
//     任何组件都不会继续持有提供者已消失的 service。

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

    // ---- 整个宿主的生命周期 --------------------------------------------

    // 按声明顺序把所有尚未激活的组件作为一个事务激活。全部组件均已激活
    // 时抛出 nostos_error。失败时只回滚本次调用激活的那些组件，因此
    // start() 可以安全重试。
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

    // 对所有激活组件做逆序 teardown。noexcept，幂等。
    void shutdown() noexcept {
        started_ = false;
        deactivate_all();
    }

    // 从 start() 成功起、直到 shutdown() 都为 true。动态卸载不清除它 —
    // 宿主仍在运行，只是组件变少了 — 因此 start() 会把缺失的组件补回来。
    bool started() const noexcept { return started_; }

    // ---- 动态（部分）卸载 ----------------------------------------

    // 回滚一个组件，外加（传递地）依赖它所提供 service 的每个组件 —
    // 依赖方在前，按逆激活序。返回被卸载的组件数。noexcept：回滚绝不
    // 能失败。宿主保持 started 状态。
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

    // 撤销一个 service：卸载提供它的组件 — 因而所有依赖方先被回滚。
    // 若没有任何组件提供 id（例如一张由动态插件拥有的 published 表），
    // 依赖方仍会被回滚，调用方因此可以放心拿走提供者；完全没有任何东西
    // 挂在 id 上时返回 false。
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

    // 激活一个未激活的组件：用于激活失败后的重试，或重新激活被
    // unload()/revoke() 回滚掉的组件。抛出组件自身的异常；当其依赖当前
    // 无人提供时抛出 missing_service；失败时把该组件完全回滚。
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

    // ---- 内省 ---------------------------------------------------

    std::size_t active_components() const noexcept {
        return count_active(std::index_sequence_for<Cs...>{});
    }

    bool is_active(std::size_t index) const noexcept {
        return index < component_count && is_active_at(index, std::index_sequence_for<Cs...>{});
    }

    // 当前提供 id 的组件，没有则为 npos。
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

    // nostos_host_api::log 的落点（默认：stderr）。
    void set_log_sink(HostCore::LogSink sink) { core_.set_log_sink(std::move(sink)); }

    // 逃生口：裸 core，供不属于组件的接线使用。
    HostCore& core() noexcept { return core_; }

    // 本宿主的 C ABI 桥 — 动态插件拿到的东西。其接线保证：插件发布的
    // service，其原生依赖方会先于插件自身停用而被回滚。首次使用时创建
    // （从不加载插件的宿主零开销），并安装到当前线程，因此插件代码可以
    // 从宿主调用它的任何位置回调宿主。
    abi::HostBridge& abi_bridge() {
        if (!bridge_) {
            bridge_ = std::make_unique<abi::HostBridge>(core_);
            bridge_->set_dependent_revoker([this](std::uint64_t id) { return this->revoke(id); });
            bridge_->install();
        }
        return *bridge_;
    }

    // abi_bridge() 至少被调用过一次后为 true。
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

    // ---- 编译期检查 ---------------------------------------------

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

    // 全宿主范围的依赖契约：Provides 唯一，Requires 被覆盖。两种失败
    // 都通过实例化一个故意失败、类型名携带违规组件与 service 的类型来
    // 报告。
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

    // 把声明交给内核，使 Context 能拒绝组件自身契约未覆盖的操作：对缺失
    // 的 Provides 条目调用 provide()，以及对缺失的 — 或类型不符的 —
    // Requires 条目调用 service()/require()。
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

    // Requires 条目还携带其 C++ 类型：调用点是独立于声明来指名类型的，
    // 因此仅凭 id 相等无法发现偏离声明的取用。
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

    // ---- 激活 -------------------------------------------------------

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
            slot.scope->reset();  // 事务式：撤销 activate() 做过的一切
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
        (deactivate_one<component_count - 1 - I>(), ...);  // 逆激活序
    }

    // 停用被标记的组件，同样按逆激活序。
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
        slot.scope->reset();  // LIFO：先执行用户清理，service 注销在最后
        core_.clear_provides_for(I);
        slot.component.reset();
        slot.scope.reset();
    }

    // （传递地）依赖 root 所提供 service 的每个组件，外加 root 自身。
    // 在已记录的依赖边上求不动点：边集很小且内核单线程，简单循环胜过
    // 图结构 — 而且它不做内存分配，这一点很重要，因为 unload() 是
    // noexcept。
    void mark_with_dependents(std::size_t root,
                              std::array<bool, component_count>& mark) const noexcept {
        mark[root] = true;
        mark_closure(mark);
    }

    // （传递地）依赖 `id` 的每个组件，无论提供者是谁。用于没有组件拥有
    // 的 service — 动态插件的 published 表 — 此时没有提供者可卸载，但
    // 依赖方仍须在表消失之前回滚。返回新标记的数量。
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

    // 沿依赖边传播标记直至不动点：被标记的组件若提供了某个 service，
    // 取用过它的组件也一并标记。
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

    // ---- 计数 ---------------------------------------------------------

    template <std::size_t... I>
    std::size_t count_active(std::index_sequence<I...>) const noexcept {
        return (std::size_t{0} + ... + (slot_active<I>() ? std::size_t{1} : std::size_t{0}));
    }

    template <std::size_t... I>
    bool is_active_at(std::size_t index, std::index_sequence<I...>) const noexcept {
        return ((index == I ? slot_active<I>() : false) || ...);
    }

    HostCore core_;  // 先声明 → 最后析构（组件会引用它）
    std::tuple<Slot<Cs>...> slots_;
    std::unique_ptr<abi::HostBridge> bridge_;
    bool started_ = false;
};

}  // namespace nostos
