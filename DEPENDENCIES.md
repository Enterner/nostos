# DEPENDENCIES.md —— 第三方依赖台账

政策见 `docs/proposal-kit.md` §1（依赖分级：Tier 0 内核/ABI 永远零依赖；
Tier 1 kit 只收 header-only 单文件库；Tier 2 app/插件自由）。

| 库 | 版本 | 许可 | 形态 | 层 | 引入理由 |
|---|---|---|---|---|---|
| nlohmann/json | 3.11.3 | MIT | header-only（单文件 vendored：`third_party/nlohmann/json.hpp`，拷自 xuanwu 3rdparty） | Tier 1（nostos::config） | 健壮 JSON 解析是经典手写陷阱（转义/Unicode/数值/错误恢复），不该自己造。仅 `nostos/kit/config.hpp` 一处使用。 |
