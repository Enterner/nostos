#pragma once
// nostos L0 — Scope, the owner of reversible side effects.
//
// Guards and child scopes are appended in creation order; teardown destroys
// them strictly in reverse (LIFO). Because a component's activation sequence
// is a topological order (a service must be provided before it can be
// acquired — see Context), the reverse of that sequence is always a safe
// destruction order. There is no runtime dependency graph: the invariant is
// enforced by construction.
//
// A Scope is non-copyable and non-movable so that references returned by
// spawn() and own() stay valid for the Scope's lifetime.

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "nostos/guard.hpp"

namespace nostos {

class Scope : public Disposable {
public:
    Scope() = default;

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    ~Scope() override { reset(); }

    // Take exclusive ownership of a guard. Pass an rvalue (std::move) or a
    // copyable guard; returns a reference to the boxed guard, valid until the
    // scope unwinds.
    template <typename G>
        requires Ownable<G>
    std::decay_t<G>& own(G&& g) {
        using GT = std::decay_t<G>;
        static_assert(std::is_rvalue_reference_v<G&&> || std::is_copy_constructible_v<GT>,
                      "nostos::Scope::own takes ownership: pass an rvalue (std::move it) "
                      "or a copyable guard");
        auto box = std::make_unique<detail::GuardBox<GT>>(GT(std::forward<G>(g)));
        GT& ref = box->guard();
        owned_.push_back(std::move(box));
        return ref;
    }

    // Poison overload. For a guard that is not Ownable, the constrained overload
    // above is simply not a candidate — what the user sees is a concept mismatch
    // ("no matching overload", "the concept Ownable<X> evaluated to false")
    // instead of the reason. This overload makes the reason reachable: by
    // construction, reaching its body is already an error, so the message below
    // is what gets printed.
    template <typename G>
        requires (!Ownable<G>)
    std::decay_t<G>& own(G&&) {
        // This overload is selected only when Ownable<G> is false, so asserting
        // the opposite is precisely the diagnostic — and it is dependent on G,
        // so it fires on instantiation rather than at definition time.
        static_assert(Ownable<G>,
                      "nostos::Scope::own needs a guard that is either an ExplicitDisposer "
                      "(has a dispose() member) or a move-only RAII type. A copyable type "
                      "without dispose() would be released twice — once from the box and "
                      "once from the source; anything else is not a guard at all.");
        std::abort();  // unreachable: the assertion above always fails
    }

    // Convenience layer: a nullary callable becomes a disposable unit.
    // A throwing callable terminates the process — rollback must not fail.
    // (move_constructible, not nothrow-move: value-capturing `const` locals
    // makes a closure's move fall back to copying — too common to reject.)
    template <typename F>
        requires std::move_constructible<std::decay_t<F>> &&
                 std::invocable<std::decay_t<F>&>
    void defer(F&& fn) {
        owned_.push_back(
            std::make_unique<detail::DeferBox<std::decay_t<F>>>(std::decay_t<F>(std::forward<F>(fn))));
    }

    // Create a child scope owned by this one; it unwinds at its position in
    // the parent's LIFO order, cascading to the child's own contents.
    Scope& spawn() {
        auto child = std::make_unique<Scope>();
        Scope& ref = *child;
        owned_.push_back(std::move(child));
        return ref;
    }

    // Unwind everything in reverse creation order. Idempotent, noexcept.
    void reset() noexcept {
        while (!owned_.empty()) {
            std::unique_ptr<Disposable> box = std::move(owned_.back());
            owned_.pop_back();
            box->dispose();  // dispose-flavor release
            box.reset();     // destroy the box → dtor-flavor release
        }
    }

    bool empty() const noexcept { return owned_.empty(); }
    std::size_t size() const noexcept { return owned_.size(); }

    // ---- effect 记账（诊断；docs/proposal-kit.md §6.2）---------------------
    //
    // defer/own 管撤销；effect 额外给每笔副作用一个 **label**，让它能被看见：
    // get_effects() 按登记顺序返回仍然挂牌的 label（"我还剩什么没撤"）。
    // 覆盖范围：只反映本 Scope 的账本——ABI 插件的订阅表在插件自己手里。

    // 登记 ⇒ 立即执行 body；label 的摘除发生在 body 之后
    // （正常回滚或手工调用返回的 disposer，都是这个顺序）。
    void effect(std::string label, std::function<void()> body) {
        auto labels = live_labels_;
        defer([labels, label = std::move(label), body = std::move(body)]() mutable {
            auto& v = *labels;
            v.erase(std::find(v.begin(), v.end(), label));
            body();
        });
        live_labels_->push_back(std::move(label));
    }

    std::vector<std::string> get_effects() const { return *live_labels_; }

    void dispose() noexcept override { reset(); }

private:
    std::vector<std::unique_ptr<Disposable>> owned_;
    // 共享给 effect 的摘牌闭包（回滚与手工 disposer 都要能摘牌）。
    std::shared_ptr<std::vector<std::string>> live_labels_ =
        std::make_shared<std::vector<std::string>>();
};

}  // namespace nostos
