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
        order_.push_back(id);
    }

    bool remove(std::uint64_t id) noexcept {
        auto it = tables_.find(id);
        if (it == tables_.end()) return false;
        tables_.erase(it);
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

private:
    std::unordered_map<std::uint64_t, const void*> tables_;
    std::vector<std::uint64_t> order_;
};

}  // namespace nostos
