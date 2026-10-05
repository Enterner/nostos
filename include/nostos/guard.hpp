#pragma once
// nostos L0 —— Disposable 协议与 guard 装箱机制。
//
// Scope 能持有两种风格的 guard：
//   * dispose 风格：有 `dispose()` 成员，在 teardown 时恰好调用一次
//     （除非 guard 提前经其他途径释放）；
//   * dtor 风格：普通 RAII 类型（std::unique_lock、std::fstream、自定义
//     guard…），由析构函数执行释放。无需任何适配——"任意 C++ RAII 类型都是
//     一个 nostos effect" 正是靠这一点成立。
//
// 纪律：回滚不许失败。GuardBox::dispose() 是 noexcept 的；抛异常的 dispose()
// 会让进程终止，与抛异常的析构函数完全一致。dispose() 已经跑过的 guard 必须
// 能被安全析构。

#include <concepts>
#include <type_traits>
#include <utility>

namespace nostos {

// 由 Scope 持有的一份可逆工作单元。dispose() 必须 noexcept 且幂等；返回之后
// 对象必须能被安全析构、不再产生副作用（与 unique_ptr::release() 同一契约）。
class Disposable {
public:
    virtual void dispose() noexcept = 0;
    virtual ~Disposable() = default;

    Disposable(const Disposable&) = delete;
    Disposable& operator=(const Disposable&) = delete;

protected:
    Disposable() = default;
};

// 通过显式 dispose() 成员释放的 guard。抛异常的 dispose() 编译期放行、抛出
// 之时终止进程——回滚失败在设计上就不可恢复。
template <typename G>
concept ExplicitDisposer = requires(G& g) { g.dispose(); };

// Scope 能持有的一切。
//
// * ExplicitDisposer：任何形态都行——释放经 dispose() 恰好发生一次；析构被
//   移动后的源对象是无副作用的空操作。
// * dtor 风格（在析构函数里释放）：必须 MOVE-ONLY。可拷贝的 guard 会被拷贝
//   进 box，而源的析构函数会提前释放——双重释放，编译期直接拒绝。std 的
//   move-only RAII 类型（unique_lock、unique_ptr、fstream）天然满足；手写的
//   guard 要么提供 dispose()，要么提供移动构造函数加上"移动后析构安全"的
//   析构函数（普通 C++ RAII 纪律）。
template <typename G>
concept Ownable =
    ExplicitDisposer<std::decay_t<G>> ||
    (std::move_constructible<std::decay_t<G>> &&
     !std::is_copy_constructible_v<std::decay_t<G>>);

namespace detail {

template <typename G>
class GuardBox final : public Disposable {
public:
    explicit GuardBox(G&& g) : guard_(std::move(g)) {}

    G& guard() noexcept { return guard_; }

    void dispose() noexcept override { release_once(); }

    ~GuardBox() override {
        release_once();
        // ~G 在这里总会运行；对已释放的 guard 而言它没有副作用。
    }

private:
    // 唯一的释放路径，由 dispose() 与析构函数共享——两个调用点因此不会各自
    // 漂移。
    //
    // 对 dtor 风格的 guard，这里没有任何可调用的东西：释放就是 ~G 本身，
    // done_ 完全不参与判断，第二次 dispose() 只是再走一遍同一个空分支。这种
    // 情况下写它只会让人以为这个标志参与了释放决策，而事实并非如此。
    void release_once() noexcept {
        if constexpr (ExplicitDisposer<G>) {
            if (done_) return;
            done_ = true;
            guard_.dispose();  // 抛出 ⇒ std::terminate，纪律如此
        }
    }

    bool done_ = false;
    G guard_;
};

// scope.defer(fn)：无参可调用对象变成一个可释放单元。fn 抛异常即终止——
// 与 dispose() 同一纪律。
template <typename F>
class DeferBox final : public Disposable {
public:
    explicit DeferBox(F&& f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : fn_(std::move(f)) {}

    void dispose() noexcept override { run_once(); }

    ~DeferBox() override { run_once(); }

private:
    // 两条释放路径都恰好调用可调用对象一次；与 GuardBox 一样，靠这个标志让
    // 第二次调用变成空操作。
    void run_once() noexcept {
        if (done_) return;
        done_ = true;
        fn_();  // 抛出 ⇒ std::terminate，纪律如此
    }

    bool done_ = false;
    F fn_;
};

}  // namespace detail

}  // namespace nostos
