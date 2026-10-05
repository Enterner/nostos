#pragma once
// nostos-kit —— Windows 平台件（Tier 1）：UTF-8 ↔ 宽字符转换。
//
// 本头是 header-only，只依赖 Win32（仅 Windows 生效；其它平台一律返回空）。

#include <string>

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

}  // namespace nostos::kit::win
