#pragma once
// nostos —— 面向 host 代码的总头文件（umbrella header）。
//
// 刻意不被 nostos/loader.hpp 或 nostos/abi/plugin.hpp 包含：
//   * 要加载 plugin 的 host 自行添加 "nostos/loader.hpp"（并链接 nostos::loader，
//     库里唯一需要编译的部分）；
//   * *plugin* 只包含 "nostos/abi/plugin.hpp" 加共享的 interface 头，
//     因此保持精简，不需要 host 侧的机制。

#include "nostos/abi/nostos_abi.h"
#include "nostos/abi/host_api.hpp"
#include "nostos/abi/interface.hpp"

#include "nostos/component.hpp"
#include "nostos/context.hpp"
#include "nostos/di.hpp"
#include "nostos/error.hpp"
#include "nostos/event.hpp"
#include "nostos/executor.hpp"
#include "nostos/guard.hpp"
#include "nostos/host_builder.hpp"
#include "nostos/published.hpp"
#include "nostos/registry.hpp"
#include "nostos/scope.hpp"
#include "nostos/svc_id.hpp"
