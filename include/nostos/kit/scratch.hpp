#pragma once
// ===========================================================================
// nostos kit —— scratch：同步 emit 的载荷稳定缓冲（F13）
//
// 纪律：ABI 事件载荷里的 `const char*` 只保证**调用期有效**——载荷字符串必须
// 比 emit 调用活得更久。临时字符串直接 `.c_str()` 是悬垂：emit 返回前临时
// 已析构，宿主读到已释放堆（踩过: 插件侧 agent——
// `trim(brief, 200).c_str()` 在 ToolResult 语句内失效 ⇒ printf 读到不定内容，
// 回归 golden 跨运行不一致）。
//
//   // 违纪：trim 的临时在语句结束前已死
//   const ToolResult e{turn, name.c_str(), ok, trim(brief, 200).c_str()};
//   host.emit(tool_result, &e);
//
//   // F13：Scratch 在作用域内持有拷贝——作用域即寿命，地址跨 hold 稳定
//   kit::Scratch scratch;
//   const ToolResult e{turn, scratch.hold(name), ok, scratch.hold(trim(brief, 200))};
//   host.emit(tool_result, &e);
//
// 成员缓冲（`out_buf_` / `call_brief_`）也能解决，但寿命推理是全局的（"谁还
// 会写这个成员？"）；Scratch 把"载荷为何有效"写在调用点上，作用域出界即释放。
//
// 依赖：仅标准库（deque/string）——kit 头不引入 JSON。
// ===========================================================================

#include <deque>
#include <string>
#include <utility>

namespace nostos::kit {

class Scratch {
public:
    Scratch() = default;
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&) = delete;
    Scratch& operator=(Scratch&&) = delete;

    // 持有一份拷贝并返回其指针：直到本 Scratch 析构都有效，且跨后续 hold()
    // 调用稳定（std::deque 端插入不失效既有元素的引用；SSO 串的缓冲随元素
    // 本体一起稳定）。返回值只读——不要通过它改写内容。
    const char* hold(std::string s) {
        hold_.emplace_back(std::move(s));
        return hold_.back().c_str();
    }

private:
    std::deque<std::string> hold_;
};

}  // namespace nostos::kit
