#pragma once
// nostos L0 — the Disposable protocol and the guard boxing machinery.
//
// Two flavors of guards can be owned by a Scope:
//   * dispose-flavor: has a `dispose()` member, called exactly once at
//     teardown (unless the guard is released earlier by other means);
//   * dtor-flavor: an ordinary RAII type (std::unique_lock, std::fstream, a
//     custom guard…) whose destructor performs the release. No adaptation
//     needed — this is the "any C++ RAII type is a nostos effect" story.
//
// Discipline: rollback must not fail. GuardBox::dispose() is noexcept; a
// dispose() that throws terminates the process, exactly like a throwing
// destructor. Guards whose dispose() has run must be safe to destroy.

#include <concepts>
#include <type_traits>
#include <utility>

namespace nostos {

// A unit of reversible work owned by a Scope. dispose() must be noexcept and
// idempotent-safe; after it returns, the object must be safe to destroy
// without further side effects (same contract as unique_ptr::release()).
class Disposable {
public:
    virtual void dispose() noexcept = 0;
    virtual ~Disposable() = default;

    Disposable(const Disposable&) = delete;
    Disposable& operator=(const Disposable&) = delete;

protected:
    Disposable() = default;
};

// Guards that release through an explicit dispose() member. A throwing
// dispose() is accepted at compile time but terminates at throw time —
// rollback failures are not recoverable by design.
template <typename G>
concept ExplicitDisposer = requires(G& g) { g.dispose(); };

// Anything a Scope can own.
//
// * ExplicitDisposer: any shape is fine — release happens exactly once via
//   dispose(); destroying the moved-from source is a no-op.
// * Dtor-flavor (release in the destructor): must be MOVE-ONLY. A copyable
//   guard would be copied into the box and the source's destructor would
//   release early — a double release we refuse at compile time. std
//   move-only RAII types (unique_lock, unique_ptr, fstream) qualify as-is;
//   hand-written guards either get a dispose() or a move constructor with a
//   moved-from-safe destructor (the ordinary C++ RAII discipline).
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
        // ~G always runs here; for released guards this is side-effect-free.
    }

private:
    // The single release path, shared by dispose() and the destructor — keeping
    // the two call sites from drifting apart.
    //
    // For a dtor-flavor guard there is nothing to call here: the release IS ~G,
    // so done_ is not consulted at all and a second dispose() simply
    // re-evaluates the same empty branch. Writing it in that case would only
    // suggest the flag had a say in the release decision, which it does not.
    void release_once() noexcept {
        if constexpr (ExplicitDisposer<G>) {
            if (done_) return;
            done_ = true;
            guard_.dispose();  // throws → std::terminate, by discipline
        }
    }

    bool done_ = false;
    G guard_;
};

// scope.defer(fn): a nullary callable becomes a disposable unit. A throwing
// fn terminates — same discipline as dispose().
template <typename F>
class DeferBox final : public Disposable {
public:
    explicit DeferBox(F&& f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : fn_(std::move(f)) {}

    void dispose() noexcept override { run_once(); }

    ~DeferBox() override { run_once(); }

private:
    // Both release paths call the callable exactly once; like GuardBox, the flag
    // is what makes a second call a no-op.
    void run_once() noexcept {
        if (done_) return;
        done_ = true;
        fn_();  // throws → std::terminate, by discipline
    }

    bool done_ = false;
    F fn_;
};

}  // namespace detail

}  // namespace nostos
