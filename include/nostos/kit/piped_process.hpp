#pragma once
// nostos-kit —— 管道化子进程（Tier 1.5）：在 Process（纯生命周期）之上补"输出捕获"。
//
//   · stdout/stderr 匿名管道：读端不继承（SetHandleInformation 去继承位）；父进程
//     写端 spawn 后立即关闭——这是读端能等到 EOF 的前提，缺了它读取线程永挂；
//   · 进程树容器：Job Object（KILL_ON_JOB_CLOSE）——terminate_tree 含孙进程；
//     句柄关闭也是兜底树杀（超时/deactivate 路径）；
//   · 工作目录可指定（UTF-8）；标准输入继承父进程。
//
// 刻意不在本层的：
//   · 逐行分割归调用方——配套的 LineSplitter 在本头内（管道/网络分块边界不保证
//     对齐，必须按字节流喂入）；读取线程模型也归调用方（典型形态：两路读取线程
//     + 单投递线程，消除并发过 poster 边界的竞态）；
//   · 输出编码清洗归调用方——子进程字节流不保证 UTF-8（控制台代码页任意）；
//   · 环境块覆盖：延后到第一个真实消费者出现再加。届时必须把合并后的 Unicode
//     环境块**按名排序**——GetEnvironmentStringsW 本就有序，覆盖合并会打乱，
//     CreateProcessW 直接报 gle=87（踩过）。
//   · 沙箱（AppContainer/低盒令牌）是宿主的安全策略，不进 kit。
//
// 本文件是 header-only；非 Windows 平台 start 恒为 false（TODO: 需要时补 posix 分支，
// 与 process.hpp 同批）。

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

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

// 行分割器：任意分块喂入，按 \n 分行（剥 \r），残余经 flush 冲刷——最后一段无换行
// 的输出也是一行（半行丢失会让"echo hello"这类用例收不到输出）。
class LineSplitter {
public:
    template <typename F>
    void feed(const char* data, std::size_t n, F&& on_line) {
        buf_.append(data, n);
        std::size_t pos = 0;
        for (;;) {
            const std::size_t eol = buf_.find('\n', pos);
            if (eol == std::string::npos) break;
            std::string line = buf_.substr(pos, eol - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            on_line(line);
            pos = eol + 1;
        }
        buf_.erase(0, pos);
    }

    template <typename F>
    void flush(F&& on_line) {
        if (buf_.empty()) return;
        std::string line = std::move(buf_);
        buf_.clear();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        on_line(line);
    }

private:
    std::string buf_;
};

// spawn 选项。command_line 是完整 UTF-8 命令行（含程序名，shell 语义解析）。
struct PipedSpawnOptions {
    std::string cwd_utf8;              // 空 = 继承父进程工作目录
    bool create_no_window = true;      // 无控制台窗口（后台捕获形态）
};

// start 产物的一组句柄（release_handles 的返回形态）：供把生命周期移交调用方
// 自管容器。句柄语义见各访问器。
struct PipedHandles {
    void* process = nullptr;      // Windows：HANDLE（可 TerminateProcess/GetExitCodeProcess）
    void* stdout_read = nullptr;  // Windows：HANDLE（可 ReadFile；EOF = 写端全关）
    void* stderr_read = nullptr;
    void* job = nullptr;          // Windows：HANDLE（KILL_ON_JOB_CLOSE，可能为 null）
};

class PipedProcess {
public:
    // Windows 上实为 HANDLE；以 void* 暴露让本头免带平台句柄类型（与 C ABI 域外
    // 头不带 windows.h 的纪律一致——实现里才有平台头）。
    using native_handle = void*;

    PipedProcess() = default;
    ~PipedProcess() { detach(); }

    PipedProcess(const PipedProcess&) = delete;
    PipedProcess& operator=(const PipedProcess&) = delete;

    PipedProcess(PipedProcess&& other) noexcept { move_from(other); }
    PipedProcess& operator=(PipedProcess&& other) noexcept {
        if (this != &other) {
            detach();
            move_from(other);
        }
        return *this;
    }

    // 起子进程并接管其 stdout/stderr。失败返回 false 并点名系统层原因（err）。
    // NOTE: 父进程的两个管道写端在 spawn 成功与否都会关闭——写端留在父手上是
    // "读端永远等不到 EOF"的经典悬挂源。
    bool start(const std::string& command_line_utf8, const PipedSpawnOptions& options,
               std::string& err) {
#if defined(_WIN32)
        detach();
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE so_read = nullptr, so_write = nullptr, se_read = nullptr, se_write = nullptr;
        if (!CreatePipe(&so_read, &so_write, &sa, 0) ||
            !CreatePipe(&se_read, &se_write, &sa, 0)) {
            err = "CreatePipe 失败 (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        // 读端不可被子进程继承（否则子进程持读端 ⇒ 父端 ReadFile 永不 EOF）
        SetHandleInformation(so_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(se_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = so_write;
        si.hStdError = se_write;
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);   // 继承父 stdin

        std::wstring wide = win::utf8_to_wide(command_line_utf8);
        std::wstring cwd_wide = win::utf8_to_wide(options.cwd_utf8);
        if (!command_line_utf8.empty() && wide.empty()) {
            CloseHandle(so_read); CloseHandle(so_write);
            CloseHandle(se_read); CloseHandle(se_write);
            err = "命令行 UTF-8 转换失败";
            return false;
        }
        DWORD flags = options.create_no_window ? CREATE_NO_WINDOW : 0;
        PROCESS_INFORMATION pi{};
        // CreateProcess* 可能改写命令行缓冲 ⇒ 传可写副本
        const BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, flags,
                                       nullptr,
                                       cwd_wide.empty() ? nullptr : cwd_wide.c_str(),
                                       &si, &pi);
        CloseHandle(so_write);   // 父写端用完即关（EOF 的前提）
        CloseHandle(se_write);
        if (!ok) {
            CloseHandle(so_read);
            CloseHandle(se_read);
            err = "CreateProcess 失败 (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        stdout_read_ = so_read;
        stderr_read_ = se_read;
        pid_ = pi.dwProcessId;
        // Job Object（进程树容器）：KILL_ON_JOB_CLOSE ⇒ 句柄关闭即整树终止；创建
        // 失败不致命（terminate_tree 退回 TerminateProcess 只杀直接子进程）
        job_ = ::CreateJobObjectW(nullptr, nullptr);
        if (job_ != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jli{};
            jli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &jli,
                                      sizeof jli);
            ::AssignProcessToJobObject(job_, pi.hProcess);
        }
        return true;
#else
        (void)command_line_utf8;
        (void)options;
        (void)err;
        return false;
#endif
    }

    native_handle process_handle() const { return process_; }
    native_handle stdout_read() const { return stdout_read_; }
    native_handle stderr_read() const { return stderr_read_; }
    native_handle job_handle() const { return job_; }
    std::uint32_t pid() const noexcept { return pid_; }

    // 阻塞读一段输出（任一流可用；EOF/错误 ⇒ false，读取循环据此收尾）。
    static bool read_chunk(native_handle read_end, std::string& out) {
#if defined(_WIN32)
        auto h = static_cast<HANDLE>(read_end);
        if (h == nullptr) return false;
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof buf, &got, nullptr) || got == 0) return false;
        out.append(buf, got);
        return true;
#else
        (void)read_end;
        (void)out;
        return false;
#endif
    }

    // 终止进程树：Job 在则 TerminateJobObject（含孙进程），否则退回
    // TerminateProcess（只直接子进程）。
    void terminate_tree() {
#if defined(_WIN32)
        if (job_ != nullptr) {
            ::TerminateJobObject(job_, 1);
            return;
        }
        if (process_ != nullptr) ::TerminateProcess(process_, 1);
#endif
    }

    // 阻塞等待退出；返回退出码（无法等待时返回 0xFFFFFFFF）。
    std::uint32_t wait() {
#if defined(_WIN32)
        if (process_ == nullptr) return 0xFFFFFFFFu;
        ::WaitForSingleObject(process_, INFINITE);
        return exit_code();
#else
        return 0xFFFFFFFFu;
#endif
    }

    // 限时等待；超时返回 false（进程仍在跑）。
    bool wait_for(std::chrono::milliseconds timeout) {
#if defined(_WIN32)
        if (process_ == nullptr) return true;
        return ::WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) ==
               WAIT_OBJECT_0;
#else
        (void)timeout;
        return true;
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

    bool running() const {
#if defined(_WIN32)
        if (process_ == nullptr) return false;
        DWORD code = 0;
        if (!::GetExitCodeProcess(process_, &code)) return false;
        return code == STILL_ACTIVE;
#else
        return false;
#endif
    }

    // 句柄整体移交：之后本对象为空态，生命周期归调用方（对象化容器分层用）。
    PipedHandles release_handles() {
        PipedHandles out{process_, stdout_read_, stderr_read_, job_};
        process_ = nullptr;
        stdout_read_ = nullptr;
        stderr_read_ = nullptr;
        job_ = nullptr;
        pid_ = 0;
        return out;
    }

    // 放弃全部句柄（不杀不等待）。
    void detach() {
#if defined(_WIN32)
        if (stdout_read_ != nullptr) CloseHandle(stdout_read_);
        if (stderr_read_ != nullptr) CloseHandle(stderr_read_);
        if (job_ != nullptr) CloseHandle(job_);
        if (process_ != nullptr) CloseHandle(process_);
        process_ = nullptr;
        stdout_read_ = nullptr;
        stderr_read_ = nullptr;
        job_ = nullptr;
        pid_ = 0;
#endif
    }

private:
#if defined(_WIN32)
    void move_from(PipedProcess& other) noexcept {
        process_ = other.process_;
        stdout_read_ = other.stdout_read_;
        stderr_read_ = other.stderr_read_;
        job_ = other.job_;
        pid_ = other.pid_;
        other.process_ = nullptr;
        other.stdout_read_ = nullptr;
        other.stderr_read_ = nullptr;
        other.job_ = nullptr;
        other.pid_ = 0;
    }
    HANDLE process_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stderr_read_ = nullptr;
    HANDLE job_ = nullptr;
    std::uint32_t pid_ = 0;
#endif
};

}  // namespace nostos::kit
