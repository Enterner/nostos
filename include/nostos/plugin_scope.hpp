#pragma once
// nostos —— ABI 插件的 L0 门面（开发观察 F9 落地）。
//
// 插件纪律：abi/plugin.hpp + 宿主契约头是必经之路；本头把内核的**可逆副作用原语**
// 开放给 ABI 插件——只包含 L0（scope/guard），零宿主世界依赖（不含 context/
// host_builder/loader/published）。L0 是纯逻辑、零第三方依赖，不违反 Tier 0。
//
// 典型用法（插件内的多步事务："任一步失败 ⇒ LIFO 全量还原"）：
//
//     nostos::Scope undo;
//     bool committed = false;
//     for (每一步) {
//         undo.defer([=, &committed] { if (!committed) restore(该步的逆操作); });
//         do_step();                       // 任一步失败 ⇒ return 前先 undo.reset()
//     }
//     committed = true;                    // 全部成功：defer 变为空操作
//
// 纪律与内核一致：defer 的可调用体**不得抛出**（回滚不允许失败）；
// committed 检查让"成功路径"的 defer 变为无操作，作用域析构时零副作用。
//
// 不提供的东西（刻意）：Context / 服务注册 / 事件——那些是宿主世界的 L1，
// ABI 插件经由宿主契约头与 published 表交互，不走本门面。

#include "nostos/guard.hpp"
#include "nostos/scope.hpp"
