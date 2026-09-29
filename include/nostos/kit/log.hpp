#pragma once
// nostos-kit —— 日志服务（docs/proposal-kit.md §5.1）。
//
// 命名 logger + 级别过滤 + 多 sink；可选异步（有界队列 + block/drop_oldest 溢出策略）。
// 零第三方依赖（spdlog 列为 Tier 1 备选，复杂度失控再准入）。
//
// 与宿主的关系：sink 是普通回调——宿主把 sink 接到自己的日志出口
// （参考宿主：sink → host.log → app.log 事件），kit 不依赖任何宿主概念。
// ABI 插件：经 abi/nostos_services.h 的 nostos_log_v1 表取用。

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nostos::kit {

class Log {
public:
    enum Level : int { trace = 0, debug = 1, info = 2, warn = 3, error = 4 };
    enum class Overflow { block, drop_oldest };
    using Sink = std::function<void(int level, const std::string& line)>;

    // sink 收 "[name][L] text" 形态的一行。时间戳/结构化字段是使用方的职责
    // （本层保持零依赖）。
    void add_sink(Sink s) {
        std::lock_guard<std::mutex> lock(mu_);
        sinks_.push_back(std::move(s));
    }
    void clear_sinks() {
        std::lock_guard<std::mutex> lock(mu_);
        sinks_.clear();
    }

    // 全局阈值：level > 阈值 ⇒ 丢弃（不入队、不进 sink）。
    void set_level(int level) noexcept { level_.store(level, std::memory_order_relaxed); }
    int level() const noexcept { return level_.load(std::memory_order_relaxed); }

    // 同步路径：逐 sink 直调（调用线程 = sink 执行线程）。
    // 异步路径（start_async 之后）：入有界队列，后台线程送 sink。
    // 级别阈值：lv 低于阈值 ⇒ 丢弃（error(4) 阈值放行 warn 及以上）。
    void log(std::string_view name, int lv, const std::string& text) {
        if (lv < level()) return;
        const auto line = format(name, lv, text);
        if (async_.load(std::memory_order_acquire)) {
            enqueue(Item{lv, line});
            return;
        }
        dispatch(Item{lv, line});
    }

    // ---- 异步路径：有界队列 + 溢出策略 ------------------------------------
    //
    // start_async 后 log() 只入队：队列满时按策略——
    //   block       ⇒ 生产者等待（背压）；
    //   drop_oldest ⇒ 丢最旧并计数（dropped() 可查）。
    // 后台线程逐条送 sink；stop_async() 排空队列后收线程（均幂等）。
    void start_async(std::size_t max_queue, Overflow policy) {
        bool expected = false;
        if (!async_.compare_exchange_strong(expected, true)) return;
        std::lock_guard<std::mutex> lock(cv_mu_);
        max_queue_ = max_queue;
        policy_ = policy;
        dropped_.store(0, std::memory_order_relaxed);  // 每段异步会话独立计数
        worker_ = std::thread([this] { pump(); });
        cv_.notify_all();
    }
    void stop_async() {
        bool expected = true;
        if (!async_.compare_exchange_strong(expected, false)) return;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    // ---- 命名 facade -------------------------------------------------------

    class Named {
    public:
        Named(Log* log, std::string name) : log_(log), name_(std::move(name)) {}
        // 注意：Log::trace 等枚举常量与本类方法同名，必须带 Log:: 限定。
        void trace(const std::string& m) { emit(Log::trace, m); }
        void debug(const std::string& m) { emit(Log::debug, m); }
        void info(const std::string& m) { emit(Log::info, m); }
        void warn(const std::string& m) { emit(Log::warn, m); }
        void error(const std::string& m) { emit(Log::error, m); }

    private:
        void emit(int level, const std::string& m) { log_->log(name_, level, m); }
        Log* log_;
        std::string name_;
    };
    Named named(const std::string& n) { return Named(this, n); }

private:
    static const char* level_tag(int level) noexcept {
        switch (level) {
            case trace: return "T";
            case debug: return "D";
            case info: return "I";
            case warn: return "W";
            case error: return "E";
        }
        return "?";
    }
    static std::string format(std::string_view name, int level, const std::string& text) {
        std::string line;
        line.reserve(name.size() + text.size() + 8);
        line += '[';
        line += name;
        line += "][";
        line += level_tag(level);
        line += "] ";
        line += text;
        return line;
    }

    struct Item {
        int level = 0;
        std::string line;
    };

    void dispatch(const Item& item) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& s : sinks_) s(item.level, item.line);
    }

    // 有界队列入队：满时按策略 block（背压等待）或 drop_oldest（丢最旧计数）。
    void enqueue(Item&& item) {
        std::unique_lock<std::mutex> lock(cv_mu_);
        if (policy_ == Overflow::block) {
            cv_.wait(lock, [&] { return !async_ || static_cast<int>(queue_.size()) < max_queue_; });
            if (!async_) return;  // 停止中的等待者：不再入队
        } else {
            while (static_cast<int>(queue_.size()) >= max_queue_) {
                queue_.pop_front();
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        queue_.push_back(std::move(item));
        cv_.notify_all();
    }

    void pump() {
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(cv_mu_);
                cv_.wait(lock, [&] { return !async_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stop 且已排空
                item = std::move(queue_.front());
                queue_.pop_front();
                cv_.notify_all();  // 弹出释放了空位：唤醒 block 策略下等空位的生产者
            }
            dispatch(item);
        }
    }

    std::mutex mu_;                       // sinks_
    std::vector<Sink> sinks_;
    std::atomic<int> level_{info};
    std::atomic<std::uint64_t> dropped_{0};

    std::mutex cv_mu_;                    // queue_ / max_queue_ / policy_
    std::deque<Item> queue_;
    std::size_t max_queue_ = 1024;
    Overflow policy_ = Overflow::block;
    std::atomic<bool> async_{false};
    std::thread worker_;
    std::condition_variable cv_;
};

}  // namespace nostos::kit
