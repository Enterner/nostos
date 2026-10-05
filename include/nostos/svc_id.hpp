#pragma once
// nostos L0 —— 编译期 service 身份。
//
// service 与 event 用编译期哈希的名字（fnv1a-64）标识。同一个 64 位值既充当
// C++ 核心里的 registry 键，又充当跨 C ABI 边界的线上标识符——"MinGW 插件
// ↔ MSVC host" 之所以可行全在于此：typeid/std::type_index 跨编译器不稳定，
// 名字则稳定。

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nostos {

// 可用作 C++20 非类型模板参数的结构化字面字符串。
template <std::size_t N>
struct fixed_string {
    char value[N]{};

    constexpr fixed_string(const char (&s)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) value[i] = s[i];
    }

    constexpr const char* data() const noexcept { return value; }
    constexpr std::size_t size() const noexcept { return N - 1; }  // 不含 NUL
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

// service 或 event 的身份。两个名字绝不许映射到同一个 id——di.hpp 在编译期
// 对全部 Provides 做去重断言（Phase 2）。
template <fixed_string S>
inline constexpr std::uint64_t svc_id = detail::fnv1a64(S.view());

// 运行期同名哈希：宿主把 manifest 等**文本**里的服务名映射回 svc_id
// （懒激活、依赖记账）。与上面的编译期算法逐字节一致。
inline std::uint64_t svc_id_of(std::string_view name) noexcept {
    return detail::fnv1a64(name);
}

}  // namespace nostos
