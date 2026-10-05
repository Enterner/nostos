#pragma once
// nostos L2 —— published 服务：服务系统中跨越动态 plugin 边界的那一半
// （docs/design.md §4.7）。
//
// 刻意分成两层：
//   * native 服务（registry.hpp）是以 svc_id 为键的 C++ 对象——快、带类型，
//     但只在同一个 ABI 域内可达；
//   * published 服务是以同一个 svc_id 为键的 C interface 表（abi/interface.hpp）
//     ——它是另一个编译器构建的 plugin 唯一能调用的东西。
// 一个 component 可以两者都提供：共享同一身份，但不共享表示，
// 任何东西都不会在两层之间隐式转换。

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
    // 注册一张表。头在这里校验，消费者因此永远不可能解析出未盖章的表：
    // struct_size 必须覆盖头本身，abi_version 必须非零。重复 id 一律拒绝
    // （这是 duplicate_service 的 published 镜像）。
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

    // 原始表查找——nostos_host_api::service 调用的就是它。
    const void* find(std::uint64_t id) const noexcept {
        auto it = tables_.find(id);
        return it == tables_.end() ? nullptr : it->second;
    }

    // host 侧（同一 ABI 域）调用者的带类型查找，带版本门控。
    template <abi::Table T>
    const T* find_as(std::uint64_t id, std::uint32_t min_version = 1) const noexcept {
        return abi::as_table<T>(find(id), min_version);
    }

    bool contains(std::uint64_t id) const noexcept { return tables_.count(id) != 0; }
    std::size_t size() const noexcept { return tables_.size(); }

    // 按注册顺序排列的 id（自省/测试用）。
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
