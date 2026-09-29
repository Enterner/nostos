#pragma once
// nostos-kit —— 后台任务（docs/proposal-kit.md §5.4）。
//
// spawn 把 job 放到独立线程；完成/异常经 dispatch 回宿主线程。
// 关停行为随任务声明（Chromium TaskTraits 的做法）：
//   block   —— drain() 时 join（宿主退出等它做完）；
//   discard —— drain() 时 detach（job/回执自持堆内存，安全逃逸）。
// 并发上限：在跑任务达到 max_concurrent 时，spawn 等待空位（cv）。
// 计数器放共享堆状态（counters_）：discard 线程逃逸后仍能安全自减。
// 析构 = drain()。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace nostos::kit {

class Tasks {
public:
    enum class Shutdown { block, discard };
    using Dispatch = std::function<void(std::function<void()>)>;

    explicit Tasks(Dispatch dispatch, std::size_t max_concurrent = 4)
        : dispatch_(std::move(dispatch)),
          counters_(std::make_shared<Counters>()),
          limit_(max_concurrent == 0 ? 1 : max_concurrent) {}

    ~Tasks() { drain(); }

    Tasks(const Tasks&) = delete;
    Tasks& operator=(const Tasks&) = delete;

    // 完成回调 on_done 经 dispatch 投回宿主线程（可空 = 不回调）。
    void spawn(Shutdown trait, std::function<void()> job,
               std::function<void()> on_done = nullptr) {
        std::unique_lock<std::mutex> lock(counters_->mu);
        counters_->cv.wait(lock, [&] { return counters_->live < limit_; });
        ++counters_->live;
        lock.unlock();

        auto entry = std::make_shared<TaskEntry>();
        entry->trait = trait;
        entry->done = std::make_shared<Done>(dispatch_, std::move(on_done));
        // 线程自持全部状态（entry/counters 都是 shared_ptr）：
        // discard 逃逸后，Tasks 对象即使已析构也无悬垂。
        entry->thread = std::thread([counters = counters_, entry, job = std::move(job)] {
            try {
                job();
            } catch (...) {
            }
            entry->done->finish();
            std::lock_guard<std::mutex> g(counters->mu);
            --counters->live;
            counters->cv.notify_all();
        });

        std::lock_guard<std::mutex> g(counters_->mu);
        threads_.push_back(std::move(entry));
    }

    // 收尾：block 的 join（宿主退出等它做完）；discard 的 detach
    // （job/回执自持堆内存，安全逃逸；完成回调仍会送达）。
    void drain() {
        std::vector<std::shared_ptr<TaskEntry>> remaining;
        {
            std::lock_guard<std::mutex> g(counters_->mu);
            remaining.swap(threads_);
        }
        for (auto& e : remaining) {
            if (e->trait == Shutdown::block && e->thread.joinable()) e->thread.join();
            else if (e->thread.joinable()) e->thread.detach();
        }
    }

    std::size_t live() const {
        std::lock_guard<std::mutex> g(counters_->mu);
        return counters_->live;
    }

private:
    struct Done {
        Dispatch dispatch;
        std::function<void()> on_done;
        void finish() {
            if (!on_done) return;
            try {
                dispatch(on_done);
            } catch (...) {
            }
        }
    };
    struct TaskEntry {
        Shutdown trait = Shutdown::block;
        std::shared_ptr<Done> done;
        std::thread thread;
    };
    struct Counters {
        std::mutex mu;
        std::condition_variable cv;
        std::size_t live = 0;
    };

    Dispatch dispatch_;
    std::shared_ptr<Counters> counters_;
    std::size_t limit_ = 1;
    std::vector<std::shared_ptr<TaskEntry>> threads_;
};

}  // namespace nostos::kit
