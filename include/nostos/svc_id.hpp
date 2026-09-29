#pragma once
// nostos L0 — compile-time service identity.
//
// Services and events are identified by a name hashed at compile time
// (fnv1a-64). The same 64-bit value serves as the registry key in the C++
// core and as the wire identifier across the C ABI boundary, which is what
// makes "MinGW plugin ↔ MSVC host" possible: typeid/std::type_index are NOT
// stable across compilers, names are.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nostos {

// A structural literal string usable as a C++20 non-type template parameter.
template <std::size_t N>
struct fixed_string {
    char value[N]{};

    constexpr fixed_string(const char (&s)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) value[i] = s[i];
    }

    constexpr const char* data() const noexcept { return value; }
    constexpr std::size_t size() const noexcept { return N - 1; }  // excluding the NUL
    constexpr std::string_view view() const noexcept { return {value, N - 1}; }

    friend constexpr bool operator==(const fixed_string& a, const fixed_string& b) noexcept {
        for (std::size_t i = 0; i < N; ++i)
            if (a.value[i] != b.value[i]) return false;
        return true;
    }
};

namespace detail {

constexpr std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

}  // namespace detail

// Identity of a service or event. Two names must never map to the same id;
// Phase 2 adds a compile-time dedup assertion over all Provides.
template <fixed_string S>
inline constexpr std::uint64_t svc_id = detail::fnv1a64(S.view());

// 运行期同名哈希：宿主把 manifest 等**文本**里的服务名映射回 svc_id
// （懒激活、依赖记账）。与上面的编译期算法逐字节一致。
inline std::uint64_t svc_id_of(std::string_view name) noexcept {
    return detail::fnv1a64(name);
}

}  // namespace nostos
