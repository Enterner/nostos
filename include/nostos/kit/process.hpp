#pragma once
// nostos-kit —— 子进程生命周期（Tier 1）。
//
// start(command_line_utf8) 用 CreateProcessW 起一个子进程；wait/kill/exit_code
// 管它的生与死。句柄 RAII：析构 = 不再等待（分离），不会杀进程也不会挂起——
// 要"退出前等它做完"由调用方显式 wait()（与 kit::task 的 block 语义一致）。
//
// 非 Windows 平台：当前未实现（start 返回 false）。需要时再补 posix 分支。

#include <chrono>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "nostos/kit/win.hpp"  // utf8_to_wide
#endif

namespace nostos::kit {

class Process {
public:
    Process() = default;
    ~Process() { detach(); }

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    Process(Process&& other) noexcept { move_from(other); }
    Process& operator=(Process&& other) noexcept {
        if (this != &other) {
            detach();
            move_from(other);
        }
        return *this;
    }

    // 起子进程（命令行是完整 UTF-8 命令行，含程序名；经 shell 语义解析）。
    // 继承调用方的控制台；不重定向标准流。成功返回 true。
    bool start(const std::string& command_line_utf8) {
#if defined(_WIN32)
        detach();
        std::wstring wide = win::utf8_to_wide(command_line_utf8);
        if (wide.empty()) return false;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // CreateProcessW 可能改写命令行缓冲 ⇒ 传可写副本
        if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &si, &pi)) {
            return false;
        }
        process_ = pi.hProcess;
        thread_ = pi.hThread;
        pid_ = pi.dwProcessId;
        return true;
#else
        (void)command_line_utf8;
        return false;
#endif
    }

    // 阻塞等待退出；返回退出码（无法等待时返回 0xFFFFFFFF）。
    std::uint32_t wait() {
#if defined(_WIN32)
        if (process_ == nullptr) return 0xFFFFFFFFu;
        ::WaitForSingleObject(process_, INFINITE);
        DWORD code = 0xFFFFFFFFu;
        ::GetExitCodeProcess(process_, &code);
        return static_cast<std::uint32_t>(code);
#else
        return 0xFFFFFFFFu;
#endif
    }

    // 限时等待；超时返回 false（进程仍在跑）。
    bool wait_for(std::chrono::milliseconds timeout) {
#if defined(_WIN32)
        if (process_ == nullptr) return true;
        const DWORD r = ::WaitForSingleObject(process_, static_cast<DWORD>(timeout.count()));
        return r == WAIT_OBJECT_0;
#else
        (void)timeout;
        return true;
#endif
    }

    // 请求终止（TerminateProcess）。对已退出的进程是空操作。
    void kill() {
#if defined(_WIN32)
        if (process_ != nullptr) ::TerminateProcess(process_, 1);
#endif
    }

    bool running() {
#if defined(_WIN32)
        if (process_ == nullptr) return false;
        DWORD code = 0;
        if (!::GetExitCodeProcess(process_, &code)) return false;
        return code == STILL_ACTIVE;
#else
        return false;
#endif
    }

    std::uint32_t exit_code() const {
#if defined(_WIN32)
        if (process_ == nullptr) return 0xFFFFFFFFu;
        DWORD code = 0xFFFFFFFFu;
        ::GetExitCodeProcess(process_, &code);
        return static_cast<std::uint32_t>(code);
#else
        return 0xFFFFFFFFu;
#endif
    }

    std::uint32_t pid() const noexcept { return pid_; }

    // 放弃句柄（不杀不等待）。之后 running() 恒为 false。
    void detach() {
#if defined(_WIN32)
        if (thread_ != nullptr) ::CloseHandle(thread_);
        if (process_ != nullptr) ::CloseHandle(process_);
        thread_ = nullptr;
        process_ = nullptr;
        pid_ = 0;
#endif
    }

private:
#if defined(_WIN32)
    void move_from(Process& other) noexcept {
        process_ = other.process_;
        thread_ = other.thread_;
        pid_ = other.pid_;
        other.process_ = nullptr;
        other.thread_ = nullptr;
        other.pid_ = 0;
    }
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
    std::uint32_t pid_ = 0;
#endif
};

}  // namespace nostos::kit
