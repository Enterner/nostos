#pragma once
// nostos L2 — published services: the half of the service system that
// crosses the dynamic-plugin boundary (docs/design.md §4.7).
//
// Two layers, deliberately:
//   * native services (registry.hpp) are C++ objects keyed by svc_id — fast,
//     typed, but reachable only from inside one ABI domain;
//   * published services are C interface tables (abi/interface.hpp) keyed by
//     the same svc_id — the only thing a plugin built by another compiler can
//     call.
// A component may offer both; they share an identity but not a representation,
// and nothing ever converts between them implicitly.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nostos/abi/nostos_abi.h"
#include "nostos/abi/interface.hpp"
#include "nostos/error.hpp"

namespace nostos {

class PublishedRegistry {
public:
    // Register a table. The header is validated here so no consumer can ever
    // resolve an unstamped table: struct_size must cover the header itself and
    // abi_version must be non-zero. Duplicate ids are refused (this is the
    // published mirror of duplicate_service).
    void add(std::uint64_t id, const void* table, std::string_view name) {
        if (table == nullptr)
            throw bad_published_table(name, bad_published_table::Reason::null_table);
        const auto* header = static_cast<const abi::TableHeader*>(table);
        if (header->struct_size < sizeof(abi::TableHeader))
            throw bad_published_table(name, bad_published_table::Reason::struct_size,
                                      header->struct_size);
        if (header->abi_version == 0)
            throw bad_published_table(name, bad_published_table::Reason::abi_version,
                                      header->abi_version);
        if (tables_.count(id) != 0) throw duplicate_service(name, id);
        tables_.emplace(id, table);
        names_.emplace(id, std::string{name});
        order_.push_back(id);
    }

    bool remove(std::uint64_t id) noexcept {
        auto it = tables_.find(id);
        if (it == tables_.end()) return false;
        tables_.erase(it);
        names_.erase(id);  // 键即 id；勿在 erase 后经由迭代器取键（已失效）
        for (auto o = order_.begin(); o != order_.end(); ++o) {
            if (*o == id) {
                order_.erase(o);
                break;
            }
        }
        return true;
    }

    // Raw table lookup — what nostos_host_api::service calls.
    const void* find(std::uint64_t id) const noexcept {
        auto it = tables_.find(id);
        return it == tables_.end() ? nullptr : it->second;
    }

    // Typed lookup for host-side (same ABI domain) callers, version-gated.
    template <abi::Table T>
    const T* find_as(std::uint64_t id, std::uint32_t min_version = 1) const noexcept {
        return abi::as_table<T>(find(id), min_version);
    }

    bool contains(std::uint64_t id) const noexcept { return tables_.count(id) != 0; }
    std::size_t size() const noexcept { return tables_.size(); }

    // Ids in registration order (introspection/tests).
    const std::vector<std::uint64_t>& ids() const noexcept { return order_; }

    // ---- 枚举（跨 ABI 域的“发现”）-------------------------------------------
    //
    // 插件无法知道“现在都发布了谁”——宿主自省（/api/status 类端点）与插件侧
    // 工具发现（ABI 尾部追加的 published_list，见 nostos_abi.h）都需要枚举。
    // 名字随条目存储：add() 的 name 从“只在报错里出现”变为注册表的一等数据。

    struct Entry {
        std::uint64_t id;
        std::string name;
    };

    // 当前全部条目（注册顺序；名字是拷贝——宿主侧随手用，无生命周期顾虑）。
    std::vector<Entry> entries() const {
        std::vector<Entry> out;
        out.reserve(order_.size());
        for (const auto id : order_) {
            auto it = names_.find(id);
            out.push_back(Entry{id, it != names_.end() ? it->second : std::string{}});
        }
        return out;
    }

    // 名字的无拷贝借用：指针随条目存活，remove / 重新发布后失效。
    // ABI 的 published_list thunk 用它把名字指针直接交给插件（调用期/条目期有效）。
    const std::string* name_of(std::uint64_t id) const noexcept {
        auto it = names_.find(id);
        return it == names_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::uint64_t, const void*> tables_;
    std::unordered_map<std::uint64_t, std::string> names_;
    std::vector<std::uint64_t> order_;
};

}  // namespace nostos
