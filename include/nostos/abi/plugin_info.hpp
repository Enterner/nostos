#pragma once
// 一张插件表里"与驱动方式无关"的那部分：名字、版本、可选槽位是否存在。
//
// 单独一个头的原因和 svc_id.hpp 一样：**静态链接进来的插件**（不经 dlopen）也需要它，
// 而它不该把 loader（宿主侧动态加载、唯一需要编译的部分）拖进来。所以这里只依赖
// 冻结的 C 契约与标准库，任何一侧都能用。

#include "nostos/abi/nostos_abi.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace nostos::abi {

// version 的打包方式与 NOSTOS_PLUGIN_VERSION 一致：major<<24 | minor<<16 | patch。
inline std::string unpack_version(std::uint32_t packed) {
    return std::to_string((packed >> 24) & 0xffu) + "." + std::to_string((packed >> 16) & 0xffu) +
           "." + std::to_string(packed & 0xffffu);
}

struct PluginInfo {
    std::string name;  // 表里的静态名字，拷贝而来
    std::uint32_t version = 0;
    bool has_save_state = false;
    bool has_load_state = false;

    std::string version_string() const { return unpack_version(version); }
};

}  // namespace nostos::abi

namespace nostos {

// abi::PluginInfo 的别名：对外沿用历史名字 nostos::PluginInfo。
// 顶层 unpack_version 的别名留在 loader.hpp（那套 API 的一部分），这里不重复定义。
using PluginInfo = abi::PluginInfo;

}  // namespace nostos
