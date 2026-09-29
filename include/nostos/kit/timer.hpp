#pragma once
// nostos-kit —— 定时器：every / after / throttle / debounce。
//
// 回调经 dispatch 投递（docs/proposal-kit.md §5.3）：
//   · 原生组件：传 Context::executor() 的 post 包装；
//   · ABI 插件：传 poster 的 post 包装（§6.1）。
// 定时器线程只算"到点了"，真正的工作体在 dispatch 指定的线程上执行。
// 析构即停止（在跑的回调不受影响——它们已被投递）。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace nostos::kit {

class Timer {
public:
    using Dispatch = std::function<void(std::function<void()>)>;
    using Clock = std::chrono::steady_clock;

    explicit Timer(Dispatch dispatch) : dispatch_(std::move(dispatch)) {
        worker_ = std::thread([this] { run(); });
    }
    ~Timer() { stop(); }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    // 周期任务：返回 id，cancel(id) 幂等取消。
    std::uint64_t every(std::chrono::milliseconds interval, std::function<void()> fn) {
        return schedule(Clock::now() + interval, interval, std::move(fn));
    }
    // 单次延时任务。
    std::uint64_t after(std::chrono::milliseconds delay, std::function<void()> fn) {
        return schedule(Clock::now() + delay, std::chrono::milliseconds(0), std::move(fn));
    }
    void cancel(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& e : entries_)
            if (e.id == id) { e.cancelled = true; break; }
        cv_.notify_all();
    }

    // 节流：同 key 在 window 内至多执行一次（首调执行，其余丢弃）。
    // 返回 true = 本次执行了。
    bool throttle(const std::string& key, std::chrono::milliseconds window,
                  std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(throttle_mu_);
        const auto now = Clock::now();
        auto it = throttle_.find(key);
        if (it != throttle_.end() && now - it->second < window) return false;
        throttle_[key] = now;
        dispatch_(std::move(fn));
        return true;
    }

    // 防抖：同 key 的调用重置计时，quiet 内无新调用才执行（经 after 延迟）。
    void debounce(const std::string& key, std::chrono::milliseconds quiet,
                  std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(debounce_mu_);
        auto it = debounce_.find(key);
        if (it != debounce_.end()) { cancel(it->second); }
        debounce_[key] = after(quiet, [this, key, fn = std::move(fn)] {
            std::lock_guard<std::mutex> lock(debounce_mu_);
            debounce_.erase(key);
            fn();
        });
    }

private:
    struct Entry {
        std::uint64_t id = 0;
        Clock::time_point next{};
        std::chrono::milliseconds interval{0};
        bool repeat = false;
        bool cancelled = false;
        std::function<void()> fn;
    };

    std::uint64_t schedule(Clock::time_point next, std::chrono::milliseconds interval,
                           std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mu_);
        const std::uint64_t id = next_id_++;
        entries_.push_back(Entry{id, next, interval, interval.count() > 0, false, std::move(fn)});
        cv_.notify_all();
        return id;
    }

    void run() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mu_);
                if (stopped_) return;
                if (entries_.empty()) {
                    cv_.wait(lock);
                    if (stopped_) return;
                } else {
                    auto next_it = entries_.begin();
                    for (auto it = entries_.begin(); it != entries_.end(); ++it)
                        if (it->next < next_it->next) next_it = it;
                    cv_.wait_until(lock, next_it->next, [&] { return stopped_; });
                    if (stopped_) return;
                }
                // 摘出到点的任务（不含已取消）
                const auto now = Clock::now();
                std::vector<Entry> fired;
                for (auto it = entries_.begin(); it != entries_.end();) {
                    if (it->cancelled) {
                        it = entries_.erase(it);
                        continue;
                    }
                    if (it->next <= now) {
                        fired.push_back({0, it->next, it->interval, it->repeat, false,
                                         std::move(it->fn)});
                        if (it->repeat)
                            it->next += it->interval;
                        else
                            it = entries_.erase(it);
                    } else {
                        ++it;
                    }
                }
                lock.unlock();
                for (auto& e : fired) dispatch_(std::move(e.fn));
            }
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopped_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    Dispatch dispatch_;
    std::mutex mu_;
    std::vector<Entry> entries_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopped_ = false;

    std::mutex throttle_mu_;
    std::map<std::string, Clock::time_point> throttle_;
    std::mutex debounce_mu_;
    std::map<std::string, std::uint64_t> debounce_;
    std::uint64_t next_id_ = 1;
};

}  // namespace nostos::kit
