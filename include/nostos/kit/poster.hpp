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

#include <memory>
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

// 堆块的执行器：执行 + 自毁（吞异常）。
template <typename Block>
void poster_run_block(void* arg) noexcept {
    std::unique_ptr<Block> block(static_cast<Block*>(arg));
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

}  // namespace nostos::kit
