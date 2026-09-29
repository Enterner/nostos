#pragma once
// nostos L1 — native service registry.
//
// Services are keyed by their compile-time name hash (svc_id, see svc_id.hpp)
// — never by std::type_index, which is not stable across compilers. Within a
// single ABI domain the registry still enforces type safety: every entry
// remembers the type it was registered as, and a lookup requesting a different
// type is a loud error in debug builds rather than a silent
// static_pointer_cast — see find() below. Note the qualifier: in release that
// comparison is compiled out, because a wrong-typed acquisition is meant to be
// impossible by construction (di.hpp's compile-time contract plus Context's
// declared-Requires check). The runtime check is the debug belt to those braces.
//
// Native services are C++ objects and stay inside one ABI domain; crossing
// the C boundary is the job of *published* services (COM-style interface
// tables, Phase 3).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <utility>

#include "nostos/error.hpp"
#include "nostos/svc_id.hpp"

namespace nostos {

// The error vocabulary (nostos_error and friends) lives in nostos/error.hpp so
// that event.hpp, di.hpp and the loader can share it without depending on the
// registry.

// Shared, non-owning view of a service instance. Because it holds a
// shared_ptr, an out-of-order revoke degrades to an extended lifetime, never
// to a dangling reference.
template <typename T>
class ServiceHandle {
public:
    ServiceHandle() noexcept = default;
    explicit ServiceHandle(std::shared_ptr<T> ptr) noexcept : ptr_(std::move(ptr)) {}

    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_.get(); }
    T* get() const noexcept { return ptr_.get(); }
    std::shared_ptr<T> shared() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return static_cast<bool>(ptr_); }

private:
    std::shared_ptr<T> ptr_;
};

class ServiceRegistry {
public:
    ServiceRegistry() = default;
    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    // Register a service under a compile-time name. Throws duplicate_service
    // on id collision.
    template <fixed_string Name, typename T, typename... Args>
    T& provide(Args&&... args) {
        constexpr std::uint64_t id = svc_id<Name>;
        if (services_.count(id) != 0) throw duplicate_service(Name.view(), id);
        auto instance = std::make_shared<T>(std::forward<Args>(args)...);
        T& ref = *instance;
        services_.emplace(id, Entry{std::string(Name.view()), std::move(instance), &typeid(T)});
        return ref;
    }

    // Typed lookup; nullptr when absent. Throws type_mismatch in debug builds
    // if the stored type differs from the requested one.
    template <fixed_string Name, typename T>
    std::shared_ptr<T> find() const {
        constexpr std::uint64_t id = svc_id<Name>;
        auto it = services_.find(id);
        if (it == services_.end()) return nullptr;
#ifndef NDEBUG
        if (*it->second.type != typeid(T)) throw type_mismatch(Name.view(), typeid(T), *it->second.type);
#endif
        return std::static_pointer_cast<T>(it->second.instance);
    }

    void erase(std::uint64_t id) { services_.erase(id); }

    bool contains(std::uint64_t id) const { return services_.count(id) != 0; }
    std::size_t size() const noexcept { return services_.size(); }
    bool empty() const noexcept { return services_.empty(); }

    // Human-readable name for diagnostics; empty string for unknown ids.
    std::string name_of(std::uint64_t id) const {
        auto it = services_.find(id);
        return it == services_.end() ? std::string() : it->second.name;
    }

private:
    struct Entry {
        std::string name;
        std::shared_ptr<void> instance;
        const std::type_info* type;
    };

    std::unordered_map<std::uint64_t, Entry> services_;
};

}  // namespace nostos
