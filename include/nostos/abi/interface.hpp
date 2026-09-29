#pragma once
// nostos L2 — published service interface tables (the COM-style half of the
// dynamic boundary; docs/design.md §4.7).
//
// A *published service* is a plain C struct whose first member is a
// nostos::abi::TableHeader, followed by function pointers. Both sides of the
// boundary include the same small shared header that declares it, so a plugin
// built by a different compiler (the MinGW-plugin ↔ MSVC-host case) can call
// through the table without sharing a single C++ type: the layout is C, the
// identity is the fnv1a-64 service id, and nobody ever crosses the boundary
// with a C++ object.
//
//     // shared/logger_iface.h — included by host and plugin alike
//     struct LoggerTable {
//         nostos::abi::TableHeader header;
//         void (*log)(LoggerTable* self, const char* msg);
//         int  (*level)(const LoggerTable* self);
//     };
//
// Growth rules (the same discipline as nostos_abi.h, one level down):
//   * Publisher fills header.struct_size = sizeof(its own struct) and
//     header.abi_version = the interface revision it implements.
//   * Consumer requires struct_size >= sizeof(consumer's struct) and
//     abi_version >= the revision it needs. Fields are only ever appended, so
//     an old consumer accepts a new publisher, while a new consumer cleanly
//     refuses an old publisher instead of reading past the end.
//   * query() never trusts a table it has not gated; a refused table comes
//     back as NULL rather than as a crash.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <nostos/abi/nostos_abi.h>

namespace nostos::abi {

// Every published table begins with exactly this. Two uint32_t so the layout
// is identical in every compiler and on both 32/64-bit targets.
struct TableHeader {
    std::uint32_t struct_size = 0; /* sizeof(publisher's struct)    */
    std::uint32_t abi_version = 0; /* interface revision, from 1 up */
};

// The status enum as text — every diagnostic that reports a nostos_status
// should say its name, not just its number.
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

// True when T looks like a published table: standard layout with a
// TableHeader member (as_table/init_table assert it is the *first* member, so
// a reader may inspect struct_size before knowing anything else).
template <typename T>
concept Table = std::is_standard_layout_v<T> && requires(const T& t) {
    { t.header.struct_size } -> std::convertible_to<std::uint32_t>;
    { t.header.abi_version } -> std::convertible_to<std::uint32_t>;
};

// Publisher side: stamp a table. Call once, before publishing.
//
// Stamping is NOT lifetime: the host registry stores the pointer and never owns
// the table (published.hpp), so a stamped table that goes out of scope leaves a
// dangling interface behind — and `as_table` cannot tell. Statics and members
// live long enough; a local does not. Context::publish_static() makes the
// compiler check the static-storage case (see P3-4 in docs/known-issues.md).
template <Table T>
void init_table(T& table, std::uint32_t abi_version = 1) noexcept {
    static_assert(offsetof(T, header) == 0,
                  "nostos: a published table's TableHeader must be its first member");
    table.header.struct_size = static_cast<std::uint32_t>(sizeof(T));
    table.header.abi_version = abi_version;
}

// Publisher side: a table is publishable only once it was stamped.
template <Table T>
bool table_is_valid(const T& table) noexcept {
    return table.header.struct_size >= sizeof(TableHeader) &&
           table.header.abi_version != 0;
}

// Raw (already resolved) table pointer -> typed table, or nullptr when the
// publisher's revision cannot satisfy this consumer. Two independent gates: the
// publisher must have filled at least sizeof(T) (struct_size — this is the one
// that actually prevents reading past the end of the publisher's struct) and must
// declare at least min_version. min_version defaults to 1, i.e. "any revision";
// the intent usually described as "I need everything my struct declares" is
// expressed by the struct_size gate, not by min_version.
template <Table T>
const T* as_table(const void* raw, std::uint32_t min_version = 1) noexcept {
    static_assert(offsetof(T, header) == 0,
                  "nostos: a published table's TableHeader must be its first member");
    if (raw == nullptr) return nullptr;
    const auto* header = static_cast<const TableHeader*>(raw);
    if (header->struct_size < sizeof(T)) return nullptr;   /* older publisher */
    if (header->abi_version < min_version) return nullptr; /* older revision  */
    return static_cast<const T*>(raw);
}

// Consumer side: resolve a published service and gate it in one step.
template <Table T>
const T* query(const nostos_host_api* host, std::uint64_t svc_id,
               std::uint32_t min_version = 1) noexcept {
    if (host == nullptr || host->service == nullptr) return nullptr;
    /* A host built with an older ABI cannot be trusted to fill the fields
     * this plugin was compiled against; a newer host is fine (growth is
     * backward compatible, discovered through struct_size). */
    if (host->struct_size < NOSTOS_HOST_API_V1_SIZE) return nullptr;
    if (host->abi_version < NOSTOS_ABI_VERSION) return nullptr;
    return as_table<T>(host->service(svc_id), min_version);
}

}  // namespace nostos::abi
