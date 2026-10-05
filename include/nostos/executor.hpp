#pragma once
// nostos L0 —— Executor 接缝。
//
// 核心按设计单线程（见 docs/design.md §4.8）：激活、teardown 与 event
// dispatch 全部跑在 host 提供的 Executor 上。文件监听、定时器与工作线程只向
// 它 post()。这套接口从 Phase 1 起就存在，因此以后引入多线程只是换一个
// Executor，而不是重新设计。

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

// 在调用者的栈上立即执行任务。
class InlineExecutor final : public Executor {
public:
    InlineExecutor() = default;
    // 无状态，但拷贝一个 executor 永远没有意义：它只有唯一的所有者驱动，
    // 第二份拷贝会把同样的工作再驱动一遍。
    InlineExecutor(const InlineExecutor&) = delete;
    InlineExecutor& operator=(const InlineExecutor&) = delete;
    InlineExecutor(InlineExecutor&&) = default;
    InlineExecutor& operator=(InlineExecutor&&) = default;

    void post(std::function<void()> task) override { task(); }
};

// 把任务排队，直到调用 drain()；对测试和自建主循环自行驱动的 host 有用。
//
// 刻意 move-only：未声明任何拷贝操作的类会隐式生成拷贝构造，而拷贝会复制出
// 第二份待处理队列——每条排队的副作用都会随每份拷贝各跑一遍，且没有任何
// 诊断。副作用队列有且只有一个所有者。
//
// drain() 是不动点，不是有界泵：只要队列非空就一直跑，因此一个会再次 post
// 的任务（自我重排程的任务）只要持续投递，就能把 drain 一直延长。这就是
// "drain" 的本义；每个 tick 需要有界工作量的 host 传一个 budget——drain(k)
// 至多跑 k 个任务——然后自行决定何时回来。没有 budget 就根本无法驱动一个
// 自我重排程的队列，这个参数正为此存在。
class QueueExecutor final : public Executor {
public:
    static constexpr std::size_t no_budget = static_cast<std::size_t>(-1);

    QueueExecutor() = default;
    QueueExecutor(const QueueExecutor&) = delete;
    QueueExecutor& operator=(const QueueExecutor&) = delete;
    QueueExecutor(QueueExecutor&&) = default;
    QueueExecutor& operator=(QueueExecutor&&) = default;

    void post(std::function<void()> task) override { queue_.push(std::move(task)); }

    // 按 FIFO 跑排队的任务，直到队列为空或已跑 `budget` 个为止，返回实际跑
    // 的个数。无参 drain() 即无预算形式，跑到队列空为止。
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

// 包装另一个 executor 并隔离任务失败：抛异常的任务上报给 sink，而不是沿调用
// 栈 unwind 进驱动队列的一方，让一个坏任务不至于在泵的中途把 host 带崩。核心
// 自身的纪律是回滚不许失败（scope.hpp），因此抛异常的任务一定是用户代码里的
// bug——这个包装只是把它从 terminate() 变成一个*被上报的* bug。
//
// 多线程备注（README "单线程核心"）：跨线程的 executor 就是这个形态——包装
// 器不拥有队列也不拥有锁，只在内层 executor 排队的基础上加一层失败隔离。
//
// sink 按值拷贝进每个任务，而不是按引用捕获：排队的任务可能比本对象活得久
// （队列归内层 executor 所有），悬垂的 `this` 会把安全网变成崩溃。
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
