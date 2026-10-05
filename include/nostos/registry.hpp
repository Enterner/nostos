#pragma once
// nostos L1 —— 原生 service registry。
//
// service 以编译期名字哈希（svc_id，见 svc_id.hpp）作键——绝不用
// std::type_index，它跨编译器不稳定。在单个 ABI 域内，registry 仍然强制类型
// 安全：每个条目记住自己注册时的类型，查找时请求了不同类型，在 debug 构建里
// 就是显式报错，而不是无声的 static_pointer_cast——见下面的 find()。注意限定
// 语：release 构建里该比较会被编译掉，因为类型错误的取用在构造上就应当不可
// 能发生（di.hpp 的编译期契约加上 Context 的 declared-Requires 检查）。运行期
// 检查只是这套约束之外再加的一道 debug 保险（belt-and-braces）。
//
// 原生 service 是 C++ 对象，只存在于单个 ABI 域之内；跨越 C 边界是 *published*
// service（COM 风格接口表，Phase 3）的职责。

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

// 错误词汇表（nostos_error 及其家族）放在 nostos/error.hpp，event.hpp、
// di.hpp 与 loader 因此可以共享它，而不必依赖 registry。

// service 实例的共享、非独占视图。因为持有 shared_ptr，乱序 revoke 的最坏
// 结果只是生命周期被延长，绝不会变成悬垂引用。
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

    // 以编译期名字注册一个 service。id 冲突时抛 duplicate_service。
    template <fixed_string Name, typename T, typename... Args>
    T& provide(Args&&... args) {
        constexpr std::uint64_t id = svc_id<Name>;
        if (services_.count(id) != 0) throw duplicate_service(Name.view(), id);
        auto instance = std::make_shared<T>(std::forward<Args>(args)...);
        T& ref = *instance;
        services_.emplace(id, Entry{std::string(Name.view()), std::move(instance), &typeid(T)});
        return ref;
    }

    // 类型化查找；不存在时返回 nullptr。debug 构建里，存储类型与请求类型不符
    // 时抛 type_mismatch。
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

    // 供诊断用的可读名字；未知 id 返回空字符串。
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
