#pragma once
// nostos-kit —— Windows 平台件（Tier 1）：文件选择对话框 + UTF-8 ↔ 宽字符。
//
// 提取自 video_stream 的 file_dialog：浏览器出于安全不给网页真实路径，
// "选文件"这一步交给操作系统自带的文件管理器。
// 非 Windows 平台降级为"无原生选择器"（页面据此隐藏按钮），语义与原夹具一致。
//
// 本头是 header-only。注意：打开对话框需要链接 comdlg32——CMake 消费方写
// target_link_libraries(x PRIVATE comdlg32)（仅 Windows）。

#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
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

// 本平台是否提供原生选择对话框（Windows = true；其它平台页面会隐藏按钮）。
inline bool has_native_picker() {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

// 弹出"打开文件"对话框，返回用户选中的绝对路径（UTF-8）；取消/失败返回空串。
//
// filter 是成对的"描述|模式|描述|模式|"（竖线分隔，UTF-8）——内部转成 API 要的
// NUL 分隔形态；空串 ⇒ 单一"所有文件"。initial_dir 为空时由对话框自行决定起始目录。
inline std::string open_file_dialog(const std::string& initial_dir = {},
                                    const std::string& filter = {}) {
#if defined(_WIN32)
    // 把 "描述|模式|..." 转成 API 要的 "描述\0模式\0...\0\0"（宽字符）
    std::wstring wide_filter;
    {
        const std::string src = filter.empty() ? "所有文件|*.*|" : filter;
        std::string piece;
        for (const char c : src) {
            if (c == '|') {
                wide_filter += utf8_to_wide(piece);
                wide_filter += L'\0';
                piece.clear();
            } else {
                piece += c;
            }
        }
        if (!piece.empty()) {
            wide_filter += utf8_to_wide(piece);
            wide_filter += L'\0';
        }
        wide_filter += L'\0';  // 过滤器表以双 NUL 结尾
    }

    // Explorer 式对话框走老接口，长路径需要自备大缓冲（32 KiB wchar 足够）
    std::vector<wchar_t> buffer(32768, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = wide_filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    const std::wstring initial = utf8_to_wide(initial_dir);
    if (!initial.empty()) ofn.lpstrInitialDir = initial.c_str();

    if (GetOpenFileNameW(&ofn) == FALSE) return {};  // 取消或失败：调用方按"未选择"处理
    return wide_to_utf8(buffer.data());
#else
    (void)initial_dir;
    (void)filter;
    return {};
#endif
}

}  // namespace nostos::kit::win
