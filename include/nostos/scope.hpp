#pragma once
// nostos L0 —— Scope，可逆副作用的所有者。
//
// guard 与子 scope 按创建顺序尾部追加；teardown 严格按逆序（LIFO）销毁它们。
// 组件的激活序列是一个拓扑序（service 必须先被提供才能被取用——见 Context），
// 因此该序列的逆序必然是安全的销毁顺序。运行期没有依赖图：不变量靠构造方式
// 保证。
//
// Scope 不可拷贝、不可移动，使 spawn() 与 own() 返回的引用在 Scope 的整个
// 生命周期内保持有效。

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

    // 取得 guard 的独占所有权：传右值（std::move）或可拷贝的 guard。返回装箱
    // 后 guard 的引用，在 scope unwind 之前有效。
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

    // 毒化重载（poison overload）。guard 不满足 Ownable 时，上面那个受约束的
    // 重载根本不会进入候选集——用户看到的只是概念不匹配（"no matching
    // overload"、"the concept Ownable<X> evaluated to false"），看不到真正
    // 的原因。这个重载让原因变得可见：能走到它的函数体本身就已是错误，所以
    // 最终打印出来的是下面这条信息。
    template <typename G>
        requires (!Ownable<G>)
    std::decay_t<G>& own(G&&) {
        // 该重载只在 Ownable<G> 为 false 时被选中，因此断言其反面恰恰就是
        // 诊断本身；断言依赖 G，所以在实例化时触发，而不是定义时。
        static_assert(Ownable<G>,
                      "nostos::Scope::own needs a guard that is either an ExplicitDisposer "
                      "(has a dispose() member) or a move-only RAII type. A copyable type "
                      "without dispose() would be released twice — once from the box and "
                      "once from the source; anything else is not a guard at all.");
        std::abort();  // 不可达：上面的断言必然失败
    }

    // 便利层：无参可调用对象变成一个可释放单元。可调用对象抛异常即终止进程
    // ——回滚不许失败。（要求 move_constructible 而非 nothrow-move：按值捕获
    // `const` 局部量会让闭包的移动退化为拷贝——太常见，不宜拒绝。）
    template <typename F>
        requires std::move_constructible<std::decay_t<F>> &&
                 std::invocable<std::decay_t<F>&>
    void defer(F&& fn) {
        owned_.push_back(
            std::make_unique<detail::DeferBox<std::decay_t<F>>>(std::decay_t<F>(std::forward<F>(fn))));
    }

    // 创建一个由本 Scope 持有的子 scope；它在父 LIFO 顺序中的位置上 unwind，
    // 并级联到子 scope 自己的内容。
    Scope& spawn() {
        auto child = std::make_unique<Scope>();
        Scope& ref = *child;
        owned_.push_back(std::move(child));
        return ref;
    }

    // 按创建的逆序 unwind 全部内容。幂等，noexcept。
    void reset() noexcept {
        while (!owned_.empty()) {
            std::unique_ptr<Disposable> box = std::move(owned_.back());
            owned_.pop_back();
            box->dispose();  // dispose 风格的释放
            box.reset();     // 析构 box ⇒ dtor 风格的释放
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
