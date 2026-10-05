// nostos L2 —— loader 的平台部分。所有触及 <windows.h>、<dlfcn.h> 或进程映像的
// 代码都住在这里，nostos/loader.hpp 因此对包含它的每个翻译单元保持干净
// （见该头的头注释）。
//
// include 顺序很重要，不要"整理"它：所有 nostos 头在最前，<windows.h> 在最后。
// 完整的 <windows.h> 会拖进 COM 头，其中的 `#define interface struct` 会破坏
// nostos::Context::interface；它还会把 Win32 API 名字放进全局命名空间，与普通
// 标识符相撞。WIN32_LEAN_AND_MEAN 避免拉进 COM 那一半；而顺序安排正是其余部分
// 得以安全的原因（见 README，设计纪律 6）。

#include "nostos/loader.hpp"

#include "nostos/abi/host_api.hpp"
#include "nostos/abi/interface.hpp"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <vector>
#  endif
#endif

namespace nostos {
namespace {

#if defined(_WIN32)

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = static_cast<int>(utf8.size());
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, wide.data(), needed);
    return wide;
}

std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = static_cast<int>(wide.size());
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

std::string last_error_text(const std::string& path) {
    const auto code = static_cast<int>(::GetLastError());
    // Windows 的消息模板带位置参数（%1、%2 …），而 std::system_category().message()
    // 填不了它们：返回的文本里占位符原样保留——"%1 is not a valid Win32
    // application."。FormatMessageW(FORMAT_MESSAGE_ARGUMENT_ARRAY) 确实能替换，
    // 但它同时会以系统 UI 语言渲染文本，那样诊断信息里 OS 的那一半就会因机器
    // 而异。所以这里用文本替换：对这里报告的 LoadLibrary 失败，%1 就是模块路径。
    std::string text = std::system_category().message(code);
    for (std::size_t pos = text.find("%1"); pos != std::string::npos;
         pos = text.find("%1", pos + path.size())) {
        text.replace(pos, 2, path);
    }
    return text;
}

void* platform_open(const std::string& path, std::string* error) {
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR：plugin 自身的依赖在 plugin 旁边查找，
    // 而不只是 host 可执行文件旁边。
    //
    // 跨工具链的 plugin（MinGW 构建、MSVC host）通常需要自己那套运行时 DLL
    // （libstdc++-6.dll、libgcc_s_seh-1.dll、……），它们所在的目录虽然在 PATH
    // 上，却*不是*上述标志覆盖的搜索路径，所以现代调用失败时会退回旧式搜索
    // 重试。这样"把 plugin 工具链的运行时放进 PATH"就能成立——对 plugin 作者
    // 来说这是最不意外的做法。
    const std::wstring wide = to_wide(path);
    HMODULE module = ::LoadLibraryExW(wide.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) module = ::LoadLibraryW(wide.c_str());
    if (module == nullptr && error != nullptr) *error = last_error_text(path);
    return reinterpret_cast<void*>(module);
}

void platform_close(void* handle) noexcept {
    if (handle != nullptr) ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
}

void* platform_symbol(void* handle, const char* name) noexcept {
    if (handle == nullptr) return nullptr;
    return reinterpret_cast<void*>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
}

std::string platform_executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);  // 被截断：扩容后重试
    }
    const std::size_t slash = buffer.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return to_utf8(buffer.substr(0, slash));
}

const char* platform_module_extension() noexcept { return ".dll"; }

// NOTE: Windows 文件名大小写不敏感，"x.DLL" 已带扩展名，不能再追加第二个。
// 踩过: 追加第二个扩展名拼出不存在的路径，报错为"找不到模块"。
bool platform_name_has_extension(std::string_view name) noexcept {
    const std::string_view extension = platform_module_extension();
    if (name.size() < extension.size()) return false;
    const std::string_view tail = name.substr(name.size() - extension.size());
    for (std::size_t i = 0; i < extension.size(); ++i) {
        const auto a = static_cast<unsigned char>(tail[i]);
        const auto b = static_cast<unsigned char>(extension[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

#else  // POSIX

void* platform_open(const std::string& path, std::string* error) {
    ::dlerror();  // 清掉上次残留的错误状态
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr && error != nullptr) {
        const char* message = ::dlerror();
        *error = message != nullptr ? message : "dlopen failed";
    }
    return handle;
}

void platform_close(void* handle) noexcept {
    if (handle != nullptr) ::dlclose(handle);
}

void* platform_symbol(void* handle, const char* name) noexcept {
    if (handle == nullptr) return nullptr;
    return ::dlsym(handle, name);
}

std::string platform_executable_directory() {
    char buffer[4096];
#  if defined(__APPLE__)
    std::uint32_t size = static_cast<std::uint32_t>(sizeof buffer);
    if (::_NSGetExecutablePath(buffer, &size) != 0) return {};
#  else
    const ssize_t written = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (written <= 0) return {};
    buffer[written] = '\0';
#  endif
    std::string path{buffer};
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}

const char* platform_module_extension() noexcept {
#  if defined(__APPLE__)
    return ".dylib";
#  else
    return ".so";
#  endif
}

// POSIX 文件名大小写敏感——".SO" 确实是另一个文件——所以这里保持精确比较。
bool platform_name_has_extension(std::string_view name) noexcept {
    const std::string_view extension = platform_module_extension();
    return name.size() >= extension.size() &&
           name.substr(name.size() - extension.size()) == extension;
}

#endif

}  // namespace

std::string executable_directory() { return platform_executable_directory(); }

std::string plugin_module_path(std::string_view name) {
    std::string path = platform_executable_directory();
    if (!path.empty()) path += '/';
    path.append(name);
    // 名字是否已带扩展名是平台问题：Windows 上大小写不敏感，POSIX 上精确匹配。
    if (!platform_name_has_extension(name)) path += platform_module_extension();
    return path;
}

// unpack_version 定义在 abi/plugin_info.hpp（inline）：静态链接插件的宿主也要用它，
// 而那时不该有 loader 被链接进来。

// ---- DynamicLibrary --------------------------------------------------------

DynamicLibrary::~DynamicLibrary() { close(); }

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)) {
    other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        path_ = std::move(other.path_);
        other.handle_ = nullptr;
    }
    return *this;
}

DynamicLibrary DynamicLibrary::open(const std::string& path) {
    std::string error;
    void* handle = platform_open(path, &error);
    if (handle == nullptr)
        throw plugin_error("nostos: cannot load plugin module '" + path + "': " + error);
    DynamicLibrary library;
    library.handle_ = handle;
    library.path_ = path;
    return library;
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    return platform_symbol(handle_, name);
}

void DynamicLibrary::close() noexcept {
    if (handle_ == nullptr) return;
    platform_close(handle_);
    handle_ = nullptr;
}

// ---- DynamicPlugin ---------------------------------------------------------

namespace {

using PluginEntryFn = const nostos_plugin_api* (*)();

const char* kEntrySymbol = "nostos_plugin_entry";

}  // namespace

DynamicPlugin::~DynamicPlugin() { unload(); }

DynamicPlugin::DynamicPlugin(DynamicPlugin&& other) noexcept
    : library_(std::move(other.library_)), session_(std::move(other.session_)) {}

DynamicPlugin& DynamicPlugin::operator=(DynamicPlugin&& other) noexcept {
    if (this != &other) {
        unload();
        library_ = std::move(other.library_);
        session_ = std::move(other.session_);
    }
    return *this;
}

DynamicPlugin DynamicPlugin::load(const std::string& path) {
    DynamicLibrary library = DynamicLibrary::open(path);

    // 穿越边界的自始至终只有这一个符号：一个返回 C 表的 C 函数。
    const auto entry = reinterpret_cast<PluginEntryFn>(library.symbol(kEntrySymbol));
    if (entry == nullptr)
        throw plugin_error("nostos: plugin '" + path + "' does not export " + kEntrySymbol +
                           "() — was it built with NOSTOS_PLUGIN(...) and NOSTOS_BUILDING_PLUGIN?");

    const nostos_plugin_api* api = nullptr;
    try {
        api = entry();
    } catch (...) {
        throw plugin_error("nostos: plugin '" + path +
                           "' let an exception escape nostos_plugin_entry(); the ABI forbids "
                           "exceptions across the boundary");
    }

    DynamicPlugin plugin;
    plugin.library_ = std::move(library);
    // 表校验与会话逻辑与"静态链接进来的插件"共用（abi/plugin_session.hpp）：
    // load() 只负责"把表弄到手"，其余语义两种编译模式逐字相同。
    plugin.session_.attach(api, path);
    return plugin;
}

// 下面四个都是转发：真正的校验/会话/回滚在 abi::PluginSession 里（两种模式共用）。

void DynamicPlugin::activate(abi::HostBridge& host) { session_.activate(host); }

void DynamicPlugin::deactivate() noexcept { session_.deactivate(); }

bool DynamicPlugin::save_state(void* buf, std::size_t capacity, std::size_t* out_len) const noexcept {
    return session_.save_state(buf, capacity, out_len);
}

bool DynamicPlugin::load_state(const void* buf, std::size_t len) noexcept {
    return session_.load_state(buf, len);
}

void DynamicPlugin::unload() noexcept {
    session_.reset();  // 逻辑卸载先行：绝不 unmap 一个还活着的插件
    library_.close();
}

// ---- LazyGate（懒激活策略，docs/proposal-kit.md §6.3）------------------------
//
// 宿主策略，不是内核机制：内核只提供"published 注册表"与"依赖者先行回滚"，
// 这里的全部工作是——activate 前对照 manifest 的 inject 记账，缺谁登记谁；
// published 每次变化后（宿主调 pump_into）重估，齐了自动拉起。
// 已激活插件的依赖被撤销时，回滚由既有语义覆盖（revoke_dependents 依赖者先行）。

bool LazyGate::submit(DynamicPlugin& plugin) {
    const nostos_plugin_api* api = plugin.api();
    if (api == nullptr)
        throw plugin_error("nostos: LazyGate::submit called with an unloaded plugin");

    const nostos_manifest* manifest =
        NOSTOS_PLUGIN_HAS(api, manifest) ? api->manifest : nullptr;

    // manifest 的 inject 就是插件的依赖边（差距清单里"插件不记录依赖边"那条，
    // 从这里开始补上：缺的依赖登记在 gate 的 PENDING 里，宿主可查询）。
    std::vector<std::uint64_t> missing;
    if (manifest != nullptr && manifest->inject != nullptr) {
        for (const char* const* name = manifest->inject; *name != nullptr; ++name) {
            const std::uint64_t id = svc_id_of(*name);
            if (bridge_->core().published().find(id) == nullptr) missing.push_back(id);
        }
    }

    if (missing.empty()) {
        plugin.activate(*bridge_);
        return true;
    }

    Pending entry;
    entry.missing = std::move(missing);
    entry.plugin = std::move(plugin);
    pending_.push_back(std::move(entry));
    return false;
}

std::vector<DynamicPlugin> LazyGate::pump_into() {
    std::vector<DynamicPlugin> activated;
    for (std::size_t i = 0; i < pending_.size();) {
        bool ready = true;
        for (std::uint64_t id : pending_[i].missing)
            if (bridge_->core().published().find(id) == nullptr) {
                ready = false;
                break;
            }
        if (!ready) {
            ++i;
            continue;
        }
        pending_[i].plugin.activate(*bridge_);
        activated.push_back(std::move(pending_[i].plugin));
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return activated;
}

}  // namespace nostos
