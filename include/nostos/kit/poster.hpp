#pragma once
// nostos-kit —— poster 投递助手：把"无捕获函数指针 + 堆载荷 + 失败自删"的
// 投递样板收敛为一个函数（docs/proposal-kit.md §6.1 的所有权约定落到代码里）。
//
// 裸用 poster 的完整套路是：定义一个堆上的载荷结构、写一个无捕获的 static
// trampoline、任务体内自删、post 失败（会话已作废 ⇒ BAD_STATE）时投递方自己
// 回收——漏掉最后一步就是停机路径的必现泄漏。本助手把四步并为一步：
//
//     nostos::kit::poster_submit(poster, [&] {
//         host_.emit(id, &payload);   // 在宿主线程、带桥执行
//     });
//
// NOSTOS_OK ⇒ 已入队：任务在宿主线程执行（期间领取者的桥可用），执行后自毁；
// 其余状态 ⇒ 未入队，载荷已就地回收。"post 失败谁删"由代码保证。
// 另一半所有权出口（F11）：本助手会为 poster 注册 kit 的**统一丢弃回收器**
// （经 poster.on_drop 槽位，幂等）——会话作废窗口内未执行的任务由宿主逐条
// 回调 dispose，闭包就地析构。插件两条路径都不漏。
// fn 执行期间抛出的异常在块内吞掉——异常永远不跨界（C ABI 纪律）。
//
// 这是便利设施不是义务（P1）；直接用裸 poster 表同样正确。约束：同一 poster
// 上的载荷应全部经本助手投递（on_drop 注册的统一回收器只认 kit 的块布局）。
// 位置说明：与 subscriptions.hpp 同层，实现在 include 树里随 core 头分发，
// 零第三方依赖，不违反 Tier 0（内核与 ABI 冻结面零依赖）。
//
// 同步孪生 poster_submit_await 在文件尾部：排到宿主线程执行并**阻塞等待其
// 完成**——供"调用线程 ≠ 宿主排水线程、又要拿着结果再继续"的场合（典型：
// 插件的 HTTP 处理线程调另一插件的服务表）。会话作废丢弃路径会唤醒等待方，
// 不会悬死；kit 块执行线程上的自我等待直接拒绝。

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include "nostos/abi/nostos_abi.h"

namespace nostos::kit {

namespace detail {

// 所有 kit 块的公共前缀：run = 执行并自毁；dispose = 不执行、只回收。
// on_drop 的统一回收器经这个前缀做类型擦除（对任意闭包类型生效）。
struct BlockHead {
    void (*run)(void*) noexcept;
    void (*dispose)(void*) noexcept;
};

// kit 块执行线程的标记（嵌套深度）：poster_submit_await 借此识别"在宿主排水
// 线程上等待宿主排水"的自我等待——当前任务不返回，drain 就不会前进到刚入队
// 的任务，是必然死锁。裸 post 入队的任务体不经过 kit 的 trampoline，检测不到
// （该残余约束由调用方保证，见 await 的注释）。
inline int& kit_poster_depth() noexcept {
    static thread_local int depth = 0;
    return depth;
}

// kit 块执行期间的线程标记（RAII，异常安全）。
struct KitPosterMark {
    int& depth;
    KitPosterMark() noexcept : depth(kit_poster_depth()) { ++depth; }
    ~KitPosterMark() noexcept { --depth; }
};

// 堆块的执行器：执行 + 自毁（吞异常）。
template <typename Block>
void poster_run_block(void* arg) noexcept {
    std::unique_ptr<Block> block(static_cast<Block*>(arg));
    KitPosterMark mark;
    try {
        block->fn();
    } catch (...) {
    }
}

// 堆块的回收器：不执行、只析构（on_drop 路径——任务未跑，载荷必须回收）。
template <typename Block>
void poster_dispose_block(void* arg) noexcept {
    std::unique_ptr<Block> block(static_cast<Block*>(arg));
}

// kit 的统一丢弃回调（注册到 poster.on_drop）：按块前缀分发到各自的 dispose。
inline void kit_block_dispose(void* arg) noexcept {
    if (arg == nullptr) return;
    static_cast<BlockHead*>(arg)->dispose(arg);
}

// ---- 同步投递（poster_submit_await）的块执行器与回收器 ---------------------

// await 块的执行器：执行 + 结果/异常交给等待方 + 唤醒 + 自毁。
// NOTE: state 用 shared_ptr——块在唤醒后即自毁，等待方醒来仍要读它；
//       done 与 ran 分开记：丢弃路径（dispose）只置 done，等待方据此区分
//       "执行完"与"确认不执行"。
template <typename Block>
void poster_run_await_block(void* arg) noexcept {
    std::unique_ptr<Block> block(static_cast<Block*>(arg));
    KitPosterMark mark;
    std::exception_ptr exc;
    try {
        block->fn();
    } catch (...) {
        exc = std::current_exception();
    }
    {
        std::lock_guard<std::mutex> lock(block->state->mu);
        block->state->ran = true;
        if (exc) block->state->exc = std::move(exc);
        block->state->done = true;
    }
    block->state->cv.notify_all();
}

// await 块的回收器（on_drop 路径——会话作废，任务永远不会执行）：必须唤醒
// 等待方，否则 poster_submit_await 悬死在 cv 上（F11 丢弃纪律的另一半）。
template <typename Block>
void poster_dispose_await_block(void* arg) noexcept {
    std::unique_ptr<Block> block(static_cast<Block*>(arg));
    {
        std::lock_guard<std::mutex> lock(block->state->mu);
        block->state->done = true;   // ran 保持 false ⇒ 等待方按未执行收场
    }
    block->state->cv.notify_all();
}

}  // namespace detail

// 把 fn（可调用对象，可带捕获）打包成堆块经 poster 投递，返回 post 的状态码。
//   NOSTOS_OK        —— 已入队：宿主线程执行后自毁；
//   NOSTOS_ERR_BAD_STATE —— poster 未领取（post 为空：activate 漏调 acquire_poster，
//                           或零初始化的 poster）——未入队，无副作用；
//   其余（会话作废的 BAD_STATE 等）—— 未入队，载荷已就地回收。
// post 的 C 契约是"无捕获函数指针 + void* 状态"，本函数负责把任意闭包适配
// 过去；fn 需可移动构造（普通 lambda 天然满足），new 失败按 C++ 惯例抛出。
template <typename F>
nostos_status poster_submit(const nostos_poster& poster, F&& fn) {
    // F12 防御：零初始化/未领取的 poster 的 post 为空——直接调用是进程级崩溃
    // （踩过: M5——runner 漏调 acquire_poster ⇒ 0xC0000005）。
    if (poster.post == nullptr) return NOSTOS_ERR_BAD_STATE;
    using Fn = std::decay_t<F>;
    struct Block {
        detail::BlockHead head;  // 前缀布局：统一回收器借此类型擦除
        Fn fn;
    };
    auto* block = new Block{
        detail::BlockHead{&detail::poster_run_block<Block>, &detail::poster_dispose_block<Block>},
        std::forward<F>(fn)};
    // F11：注册统一回收器（幂等；仅新宿主有该槽位）。会话作废窗口内未执行的
    // 任务由宿主逐条回调 dispose——post 成功但未执行的任务也会被回收。
    if (NOSTOS_POSTER_HAS(&poster, on_drop) && poster.on_drop != nullptr)
        poster.on_drop(poster.self, &detail::kit_block_dispose);
    const nostos_status st = poster.post(poster.self, block->head.run, block);
    if (st != NOSTOS_OK) delete block;
    return st;
}

// ---- 同步投递：poster_submit_await ----------------------------------------

// 同步投递：把 fn 排到宿主线程执行并**阻塞等待其完成**（与 poster_submit
// 同一块布局、同一 on_drop 统一回收器，同一 poster 上混用安全）。返回：
//   NOSTOS_OK            —— fn 已在宿主线程执行完；fn 抛出的异常在本线程
//                           重抛（两侧都是 C++ 域，不过 C ABI 边界）；
//   NOSTOS_ERR_BAD_STATE —— fn 确定不执行：poster 不可用（post 为空/会话
//                           已作废）、入队后随会话作废被丢弃（on_drop 会
//                           唤醒等待方），或自我等待防御命中（见下）。
// NOTE: 自我等待 = 在宿主排水线程上调用本函数——当前任务不返回，drain 就
//       不会前进，是必然死锁。kit 块执行线程上直接返回 BAD_STATE；裸 post
//       入队的任务体检测不到，残余约束由调用方保证：等待线程绝不能是宿主
//       线程自身，且宿主必须有人在排水（主循环 drain_posted 或 wake 驱动）。
// NOTE: fn 按引用捕获调用方局部变量是合法且推荐的形态——调用方阻塞到 fn
//       执行完（或确认不执行）才继续，闭包生命周期天然覆盖任务体。
template <typename F>
nostos_status poster_submit_await(const nostos_poster& poster, F&& fn) {
    if (poster.post == nullptr) return NOSTOS_ERR_BAD_STATE;   // F12 同款防御
    if (detail::kit_poster_depth() > 0) return NOSTOS_ERR_BAD_STATE;
    using Fn = std::decay_t<F>;
    struct AwaitState {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        bool ran = false;
        std::exception_ptr exc;
    };
    auto state = std::make_shared<AwaitState>();
    struct Block {
        detail::BlockHead head;  // 前缀布局：统一回收器借此类型擦除
        Fn fn;
        std::shared_ptr<AwaitState> state;
    };
    auto* block = new Block{
        detail::BlockHead{&detail::poster_run_await_block<Block>,
                          &detail::poster_dispose_await_block<Block>},
        std::forward<F>(fn), state};
    if (NOSTOS_POSTER_HAS(&poster, on_drop) && poster.on_drop != nullptr)
        poster.on_drop(poster.self, &detail::kit_block_dispose);
    const nostos_status st = poster.post(poster.self, block->head.run, block);
    if (st != NOSTOS_OK) {
        delete block;   // 未入队：投递方就是等待方，状态直接带回去
        return st;
    }
    std::unique_lock<std::mutex> lock(state->mu);
    state->cv.wait(lock, [&state] { return state->done; });
    if (!state->ran) return NOSTOS_ERR_BAD_STATE;   // 会话作废，任务被丢弃
    if (state->exc) std::rethrow_exception(state->exc);
    return NOSTOS_OK;
}

}  // namespace nostos::kit
