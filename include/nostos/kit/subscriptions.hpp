#pragma once
// nostos-kit —— 订阅表助手（ABI 插件侧的 effect 记账，VS Code 的 Disposable 形态）。
//
// 把"会留下资源"的清理器收集起来，统一释放：
//
//     nostos::kit::Subscriptions subs;
//     subs.push([&] { host.raw()->off(token); });   // 每个会留下东西的调用
//     ...
//     subs.dispose();                               // deactivate：逆序统一释放
//
// 这是便利设施不是义务（P1）；内核不要求插件使用它。
// 位置说明：位于 include/nostos/kit/，随 core 头分发（本文件零依赖，不违反 Tier 0）。

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace nostos::kit {

class Subscriptions {
public:
    // 登记一个清理器（逆序释放：后登记的先撤销，与 Scope 的 LIFO 一致）。
    void push(std::function<void()> disposer) { items_.push_back(std::move(disposer)); }

    // 逆序释放全部清理器；幂等。清理器约定不抛——抛了按缺陷处理：
    // 吞掉并继续释放其余（收尾不许终止）。
    void dispose() {
        for (std::size_t i = items_.size(); i > 0; --i) {
            try {
                if (items_[i - 1]) items_[i - 1]();
            } catch (...) {
            }
            items_[i - 1] = nullptr;
        }
        items_.clear();
    }

    bool empty() const noexcept { return items_.empty(); }
    std::size_t size() const noexcept { return items_.size(); }

private:
    std::vector<std::function<void()>> items_;
};

}  // namespace nostos::kit
