#pragma once
// nostos L2 —— published 服务接口表（动态边界的 COM 风格那一半；
// docs/design.md §4.7）。
//
// *published 服务*是一个普通 C struct：首成员为 nostos::abi::TableHeader，
// 其后是函数指针。边界两侧包含声明它的同一份小型共享头，因此由不同编译器
// 构建（MinGW-plugin ↔ MSVC-host 的情形）的 plugin 也能穿过表调用，而不必
// 共享任何一个 C++ 类型：布局是 C 的，身份是 fnv1a-64 服务 id，没有人会让
// C++ 对象跨边界。
//
//     // shared/logger_iface.h —— 宿主与 plugin 都包含它
//     struct LoggerTable {
//         nostos::abi::TableHeader header;
//         void (*log)(LoggerTable* self, const char* msg);
//         int  (*level)(const LoggerTable* self);
//     };
//
// 增长规则（与 nostos_abi.h 相同的纪律，低一个层级）：
//   * 发布方填 header.struct_size = sizeof(自己的 struct)，header.abi_version
//     = 自己实现的接口修订号。
//   * 消费方要求 struct_size >= sizeof(消费方的 struct)，且 abi_version >=
//     自己需要的修订号。字段只增不改，所以老消费方接受新发布方，而新消费方
//     会干净地拒绝老发布方，而不是读越界。
//   * query() 绝不信任未经门控的表；被拒绝的表以 NULL 返回，而不是崩溃。

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <nostos/abi/nostos_abi.h>

namespace nostos::abi {

// 每张 published 表都恰好以此开头。用两个 uint32_t，使布局在每个编译器以及
// 32/64 位目标上都一致。
struct TableHeader {
    std::uint32_t struct_size = 0; /* 发布方 struct 的 sizeof */
    std::uint32_t abi_version = 0; /* 接口修订号，从 1 起 */
};

// 以文本呈现的 status 枚举——每个报告 nostos_status 的诊断都应说出它的名字，
// 而不只是它的数字。
inline const char* status_name(nostos_status status) noexcept {
    switch (status) {
        case NOSTOS_OK: return "NOSTOS_OK";
        case NOSTOS_ERR_BAD_ABI: return "NOSTOS_ERR_BAD_ABI";
        case NOSTOS_ERR_BAD_STATE: return "NOSTOS_ERR_BAD_STATE";
        case NOSTOS_ERR_INVALID_ARG: return "NOSTOS_ERR_INVALID_ARG";
        case NOSTOS_ERR_OUT_OF_MEMORY: return "NOSTOS_ERR_OUT_OF_MEMORY";
        case NOSTOS_ERR_FAILED: return "NOSTOS_ERR_FAILED";
    }
    return "NOSTOS_ERR_UNKNOWN";
}

// T 看起来像一张 published 表时为真：standard layout 且带有 TableHeader 成员
// （as_table/init_table 断言它是*首个*成员，读取方因此在了解其他任何东西之前
// 就可以检视 struct_size）。
template <typename T>
concept Table = std::is_standard_layout_v<T> && requires(const T& t) {
    { t.header.struct_size } -> std::convertible_to<std::uint32_t>;
    { t.header.abi_version } -> std::convertible_to<std::uint32_t>;
};

// 发布方侧：为表盖章。发布之前调用一次。
//
// 盖章不等于生命周期管理：宿主注册表保存指针且绝不拥有表（published.hpp），
// 因此一张离开作用域的已盖章表会留下悬垂的接口——而 `as_table` 无从分辨。
// static 与成员活得够久；局部变量不行。Context::publish_static() 让编译器
// 检查 static 存储期的情形（见 docs/known-issues.md 的 P3-4）。
template <Table T>
void init_table(T& table, std::uint32_t abi_version = 1) noexcept {
    static_assert(offsetof(T, header) == 0,
                  "nostos: a published table's TableHeader must be its first member");
    table.header.struct_size = static_cast<std::uint32_t>(sizeof(T));
    table.header.abi_version = abi_version;
}

// 发布方侧：表只有盖章之后才可发布。
template <Table T>
bool table_is_valid(const T& table) noexcept {
    return table.header.struct_size >= sizeof(TableHeader) &&
           table.header.abi_version != 0;
}

// 裸的（已解析的）表指针 -> 带类型的表；发布方的修订无法满足本消费方时返回
// nullptr。两道独立的门：发布方必须至少填满 sizeof(T)（struct_size——真正
// 防止读到发布方 struct 末尾之外的是这道门），并且必须声明至少 min_version。
// min_version 默认为 1，即“任意修订”；通常被描述为“我要我的 struct 声明的
// 全部东西”的那种意图，由 struct_size 门表达，而不是由 min_version 表达。
template <Table T>
const T* as_table(const void* raw, std::uint32_t min_version = 1) noexcept {
    static_assert(offsetof(T, header) == 0,
                  "nostos: a published table's TableHeader must be its first member");
    if (raw == nullptr) return nullptr;
    const auto* header = static_cast<const TableHeader*>(raw);
    if (header->struct_size < sizeof(T)) return nullptr;   /* 发布方更老 */
    if (header->abi_version < min_version) return nullptr; /* 修订更老 */
    return static_cast<const T*>(raw);
}

// 消费方侧：解析 published 服务并一步完成门控。
template <Table T>
const T* query(const nostos_host_api* host, std::uint64_t svc_id,
               std::uint32_t min_version = 1) noexcept {
    if (host == nullptr || host->service == nullptr) return nullptr;
    /* 以更老 ABI 构建的宿主，不能指望它填好本 plugin 编译时所依据的字段；
     * 更新的宿主没有问题（增长向后兼容，经 struct_size 发现）。 */
    if (host->struct_size < NOSTOS_HOST_API_V1_SIZE) return nullptr;
    if (host->abi_version < NOSTOS_ABI_VERSION) return nullptr;
    return as_table<T>(host->service(svc_id), min_version);
}

}  // namespace nostos::abi
