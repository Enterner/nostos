#pragma once
// nostos-kit —— Windows 平台件（Tier 1）：UTF-8 ↔ 宽字符转换、CreateProcessW
// 环境块构建、可中断睡眠。
//
// 本头是 header-only，只依赖 Win32（仅 Windows 生效；其它平台一律返回空/空转）。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nostos::kit::win {

// UTF-8 ↔ UTF-16。转换失败/空串 ⇒ 空。
inline std::wstring utf8_to_wide(const std::string& text) {
#if defined(_WIN32)
    if (text.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring wide(need > 0 ? static_cast<std::size_t>(need) : 0, L'\0');
    if (!wide.empty())
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                            need);
    return wide;
#else
    (void)text;
    return {};
#endif
}

inline std::string wide_to_utf8(const wchar_t* text) {
#if defined(_WIN32)
    if (text == nullptr) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};  // 只剩结尾 NUL = 空串
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    return out;
#else
    (void)text;
    return {};
#endif
}

// 可中断睡眠：total 时长内按 slice 分片 Sleep，cancel 置位即提前返回。后台线程的
// 停机旗标形态——整段睡眠会让「置旗 + join」的停机白等完整剩余时长。
inline void sleep_interruptible(std::chrono::milliseconds total,
                                std::chrono::milliseconds slice,
                                const std::atomic<bool>& cancel) {
#if defined(_WIN32)
    const auto deadline = std::chrono::steady_clock::now() + total;
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) return;
        const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remain <= std::chrono::milliseconds::zero()) return;
        Sleep(static_cast<DWORD>(std::min(remain, slice).count()));
    }
#else
    (void)total;
    (void)slice;
    (void)cancel;
#endif
}

// CreateProcessW 环境块构建（单一出处）：读父环境块 + 覆盖合并 + 按名排序 + 双 NUL
// 收尾。合并按名不区分大小写（仅大小写不同的名按 Windows 语义视为同一名），覆盖者
// 胜且保留父侧名字原样。排序是 CreateProcessW 的硬要求——GetEnvironmentStringsW
// 本就有序，覆盖合并按插入序拼会打乱序，CreateProcessW 直接报 gle=87（踩过）。
// 读父块失败按空父环境继续（调用方的覆盖项仍然生效）。
inline std::wstring build_env_block_w(
    const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
#if defined(_WIN32)
    struct CiLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            const std::size_t n = a.size() < b.size() ? a.size() : b.size();
            for (std::size_t i = 0; i < n; ++i) {
                const wint_t ca = towupper(a[i]), cb = towupper(b[i]);
                if (ca != cb) return ca < cb;
            }
            return a.size() < b.size();
        }
    };
    std::map<std::wstring, std::wstring, CiLess> merged;
    if (LPWCH block = ::GetEnvironmentStringsW()) {
        for (LPWCH p = block; *p != L'\0';) {
            const std::wstring kv(p);
            p += kv.size() + 1;
            const auto eq = kv.find(L'=');
            // 等号在首位的是 =C:=<目录> 形态的盘符当前目录项，非环境变量
            if (eq != std::wstring::npos && eq != 0)
                merged.emplace(kv.substr(0, eq), kv.substr(eq + 1));
        }
        ::FreeEnvironmentStringsW(block);
    }
    for (const auto& kv : overrides)
        if (!kv.first.empty()) merged[kv.first] = kv.second;
    std::wstring out;
    for (const auto& kv : merged) {
        out += kv.first;
        out.push_back(L'=');
        out += kv.second;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
#else
    (void)overrides;
    return {};
#endif
}

}  // namespace nostos::kit::win
