// nostos L2 — the platform half of the loader. Everything that touches
// <windows.h>, <dlfcn.h> or the process image lives here so that
// nostos/loader.hpp can stay clean for every translation unit that includes
// it (see the header comment there).
//
// Include order matters, do not "tidy" it: every nostos header comes first and
// <windows.h> comes last. A full <windows.h> drags in the COM headers, whose
// `#define interface struct` would break nostos::Context::interface, and it
// puts Win32 API names into the global namespace where they collide with
// ordinary identifiers. WIN32_LEAN_AND_MEAN avoids pulling in the COM half;
// the ordering is what makes the rest safe (see README, 设计纪律 6).

#include "nostos/loader.hpp"

#include "nostos/abi/host_api.hpp"
#include "nostos/abi/interface.hpp"

#include <cctype>
#include <cstddef>
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
    // Windows message templates carry positional parameters (%1, %2 …) and
    // std::system_category().message() cannot fill them: it hands back the text
    // with the placeholder intact — "%1 is not a valid Win32 application.".
    // FormatMessageW(FORMAT_MESSAGE_ARGUMENT_ARRAY) does substitute, but it also
    // renders the text in the system UI language, so the OS half of our
    // diagnostics would change language from machine to machine. Substitute
    // textually instead: for the LoadLibrary failures reported here, %1 IS the
    // module path.
    std::string text = std::system_category().message(code);
    for (std::size_t pos = text.find("%1"); pos != std::string::npos;
         pos = text.find("%1", pos + path.size())) {
        text.replace(pos, 2, path);
    }
    return text;
}

void* platform_open(const std::string& path, std::string* error) {
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR: a plugin's own dependencies are looked
    // for next to the plugin, not only next to the host executable.
    //
    // A cross-toolchain plugin (MinGW-built, MSVC host) usually needs its own
    // runtime DLLs (libstdc++-6.dll, libgcc_s_seh-1.dll, ...) from a directory
    // that is on PATH but is *not* one of the flagged search paths, so the
    // modern call is retried with the legacy search as a fallback. That makes
    // "put the plugin's toolchain runtime on PATH" work, which is the least
    // surprising thing for a plugin author.
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
        buffer.resize(buffer.size() * 2);  // truncated: grow and retry
    }
    const std::size_t slash = buffer.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return to_utf8(buffer.substr(0, slash));
}

const char* platform_module_extension() noexcept { return ".dll"; }

// Windows file names are case-insensitive, so "x.DLL" already carries the
// extension and must not get a second one appended (that produced a path that
// does not exist, reported as "module could not be found").
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
    ::dlerror();  // clear any stale error
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

// POSIX file names are case-sensitive — ".SO" really is a different file — so
// this stays an exact comparison.
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
    // Whether the name already carries the extension is a platform question:
    // case-insensitive on Windows, exact on POSIX.
    if (!platform_name_has_extension(name)) path += platform_module_extension();
    return path;
}

// unpack_version 现在定义在 abi/plugin_info.hpp（inline）：静态链接插件的宿主也要用它，
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

    // Only this one symbol ever crosses: a C function returning a C table.
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
