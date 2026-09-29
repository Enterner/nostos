#pragma once
// nostos L0 — Executor seam.
//
// The core is single-threaded by design (see docs/design.md §4.8): activation,
// teardown and event dispatch all run on whatever Executor the host provides.
// File watchers, timers and worker threads only ever post() into it. The
// interface exists from Phase 1 so multi-threading later is a change of
// executor, not a redesign.

#include <cstddef>
#include <functional>
#include <queue>
#include <stdexcept>
#include <utility>

namespace nostos {

class Executor {
public:
    virtual ~Executor() = default;
    virtual void post(std::function<void()> task) = 0;
};

// Runs tasks immediately, on the caller's stack.
class InlineExecutor final : public Executor {
public:
    InlineExecutor() = default;
    // Stateless, but copying an executor is never meaningful: exactly one owner
    // pumps it, and a second copy would pump the same work again.
    InlineExecutor(const InlineExecutor&) = delete;
    InlineExecutor& operator=(const InlineExecutor&) = delete;
    InlineExecutor(InlineExecutor&&) = default;
    InlineExecutor& operator=(InlineExecutor&&) = default;

    void post(std::function<void()> task) override { task(); }
};

// Queues tasks until drain() is called; useful for tests and for hosts that
// pump their own loop.
//
// Move-only on purpose: the copy constructor is implicitly generated for a
// class with no copy operations declared, and copying would duplicate the
// pending queue — every queued side effect would then run twice, once per copy,
// with no diagnostic. There is exactly one owner of a queue of effects.
//
// drain() is a fixpoint, not a bounded pump: it keeps running while the queue is
// non-empty, so a task that posts again (a self-rescheduling task) extends it for
// as long as it keeps posting. That is what "drain" means; a host that needs
// bounded work per tick passes a budget — drain(k) runs at most k tasks — and
// can then decide when to come back. Without a budget there is no way to pump a
// self-rescheduling queue at all, which is why the parameter exists.
class QueueExecutor final : public Executor {
public:
    static constexpr std::size_t no_budget = static_cast<std::size_t>(-1);

    QueueExecutor() = default;
    QueueExecutor(const QueueExecutor&) = delete;
    QueueExecutor& operator=(const QueueExecutor&) = delete;
    QueueExecutor(QueueExecutor&&) = default;
    QueueExecutor& operator=(QueueExecutor&&) = default;

    void post(std::function<void()> task) override { queue_.push(std::move(task)); }

    // Run queued tasks, FIFO, until the queue is empty or `budget` tasks have
    // run. Returns how many ran. drain() (no argument) is the unbounded form and
    // keeps the original behaviour.
    std::size_t drain(std::size_t budget = no_budget) {
        std::size_t n = 0;
        while (!queue_.empty() && n < budget) {
            std::function<void()> task = std::move(queue_.front());
            queue_.pop();
            task();
            ++n;
        }
        return n;
    }

    bool empty() const noexcept { return queue_.empty(); }
    std::size_t size() const noexcept { return queue_.size(); }

private:
    std::queue<std::function<void()>> queue_;
};

// Wraps another executor and isolates task failures: a task that throws is
// reported to a sink instead of unwinding into whatever pumps the queue, so one
// bad task cannot take the host down mid-pump. The core's own discipline is that
// rollback must not fail (scope.hpp), which means a throwing task is always a bug
// in user code — this makes it a *reported* bug instead of a terminate().
//
// Multi-threading note (README "单线程核心"): this is the shape a cross-thread
// executor takes — the wrapper owns no queue and no lock, it only adds failure
// isolation around whatever inner executor does the queuing.
//
// The sink is copied into each task rather than captured by reference: a queued
// task may outlive this object (the inner executor owns the queue), so a
// dangling `this` would turn the safety net into a crash.
class SafeExecutor final : public Executor {
public:
    using ErrorSink = std::function<void(const std::exception&)>;

    explicit SafeExecutor(Executor& inner, ErrorSink sink = {})
        : inner_(&inner), sink_(std::move(sink)) {}

    SafeExecutor(const SafeExecutor&) = delete;
    SafeExecutor& operator=(const SafeExecutor&) = delete;

    void post(std::function<void()> task) override {
        ErrorSink sink = sink_;
        inner_->post([task = std::move(task), sink = std::move(sink)] {
            try {
                task();
            } catch (const std::exception& e) {
                if (sink) sink(e);
            } catch (...) {
                if (sink) {
                    const std::runtime_error unknown("nostos: task threw a non-std exception");
                    sink(unknown);
                }
            }
        });
    }

private:
    Executor* inner_ = nullptr;
    ErrorSink sink_;
};

}  // namespace nostos
