# nostos

**用一句话说：它提供"可逆副作用的运行时 + 编译期契约"。**

组件（或插件）在激活时做的一切 —— 注册服务、订阅事件、开线程/端口/文件、持有锁 —— 都被
当成**可逆副作用**记账；激活失败、运行期撤销、整体关停，都只是"把这些副作用按逆序收回"。
除此之外它还提供一条**跨编译器稳定**的动态插件边界（MinGW 编译的插件塞进 MSVC 编译的宿主）。

- 标准：C++20（MSVC 2022 / GCC 10+ / Clang 12+）
- 依赖：内核**零依赖**；kit（Tier 1）只收 header-only 单文件库并 vendored（当前仅 `nlohmann/json`）
- 形态：header-only 核心 + 两个可选静态库（`nostos::loader` 加载器 / `nostos::kit_http` 服务器）+ kit 头
- 交付：`find_package(nostos)` → `nostos::core`（头文件）/ `nostos::loader` / `nostos::kit` / `nostos::kit_http`

```bash
# 在本目录（nostos/）里构建并安装 —— 库是独立工程，不需要仓库里的其它两个工程
cmake --preset release && cmake --build --preset release
cmake --install build-release --prefix <prefix> --config RelWithDebInfo
```

```cmake
find_package(nostos CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE nostos::core nostos::loader)
```

---

## 提供什么

### 1. 可逆副作用（L0）

| 头文件 | 提供 |
|---|---|
| `svc_id.hpp` | `fixed_string` + `svc_id<"名字">` / `event_id<E>`：**编译期**服务与事件身份（fnv1a-64），跨编译器稳定 |
| `guard.hpp` | `Disposable` 协议、`Ownable` 概念、guard 装箱：**任何 RAII 类型**（`unique_lock`/`fstream`/自写锁）零适配接入 |
| `scope.hpp` | `Scope`：`own(guard)` / `defer(fn)` / `spawn()` 子作用域 / `reset()`；LIFO 逆序回滚，幂等 `noexcept` |
| `plugin_scope.hpp` | **ABI 插件的 L0 门面**：把 Scope/guard 开放给动态插件（插件内多步事务"任一步失败 LIFO 还原"的入口；只含 L0，零宿主世界依赖） |
| `executor.hpp` | `Executor` 接缝（`post`）+ `InlineExecutor` / `QueueExecutor`：单线程核心的调度口 |
| `error.hpp` | `nostos_error` 家族：`missing_service` / `duplicate_service` / `type_mismatch` / `undeclared_provide` / `event_name_collision` / `plugin_error`，**消息永远点名** |

### 2. 组合层（L1）

| 头文件 | 提供 |
|---|---|
| `registry.hpp` | `ServiceRegistry`（名称哈希键 + 域内类型安检）+ `ServiceHandle`（保活句柄，撤销后也不悬垂） |
| `event.hpp` | `EventHub` / `EventBus<E, Policy>`：`EmitPolicy`（通知）/ `BailPolicy`（首个 true 短路）/ `WaterfallPolicy`（载荷逐级变换）；**快照派发**（监听器内增删订阅安全）；`Subscription` RAII 退订；**raw 通道**（`on_raw`/`emit_raw`，C ABI 的接入点） |
| `context.hpp` | `Context`：`provide<Name,T>()` / `service<Name,T>()` / `require` / `on<E>` / `emit` / `publish<Name>(table)` / `interface<Name,T>()`；`HostCore` 是运行期账本（服务、事件、published、依赖边、provide 记录、日志出口） |
| `di.hpp` | 编译期依赖契约：`Svc<Name,T>`、`Requires` / `Provides`，三条规则（Requires ⊆ ∪Provides、同 id 类型一致、Provides 唯一）+ **具名诊断** |
| `component.hpp` | 组件协议：`activate(Context&)` 事务体 + 可选 `onStop()` |
| `host_builder.hpp` | `Host<Cs...>`：声明序激活（= 拓扑序）、**激活即事务**（失败全量回滚、可重试）、逆序关停、运行期 `unload` / `revoke` / `activate`（依赖者先行回滚） |
| `published.hpp` | `PublishedRegistry`：C 接口表形式的服务（跨 ABI 域），`add/remove/find/find_as/contains/ids` |

### 3. 动态插件边界（L2）

| 头文件 | 提供 |
|---|---|
| `abi/nostos_abi.h` | **冻结**的纯 C 契约：`nostos_host_api`（宿主能力表）/ `nostos_plugin_api`（插件导出表）/ `nostos_status`；字段只在**尾部追加**，`NOSTOS_HOST_HAS(api, field)` 逐字段门控 |
| `abi/interface.hpp` | 版本化接口表：`TableHeader{struct_size, abi_version}`、`init_table` / `as_table<T>` / `query<T>` / `status_name` |
| `abi/host_api.hpp` | 宿主侧桥 `HostBridge`：7 个 thunk（`service`/`on`/`off`/`log` + `publish`/`unpublish`/`emit`），全部 `noexcept` 吞异常；发布会话（失败激活零残留）；线程局部"当前桥"；`Guard` 显式作用域 |
| `abi/plugin.hpp` | 插件侧助手：`nostos::abi::Host`（能力按 `struct_size` 门控）、RAII `Subscription`、`NOSTOS_PLUGIN_VERSION` / `NOSTOS_PLUGIN(Type)` 入口宏（静态链接模式的宿主用 `NOSTOS_PLUGIN_ENTRY_DECL()` 声明同一个符号）；插件状态由插件自己 new/delete |
| `loader.hpp` + `src/loader/loader.cpp` | **动态加载**：`DynamicLibrary`（`LoadLibraryExW`/`dlopen` 的 RAII 封装、UTF-8 路径）/ `DynamicPlugin`（校验 `struct_size`/`abi_version`、逻辑卸载、显式物理 unmap、可选 `save_state`/`load_state`） |
| `linked_plugin.hpp` | **静态链接**：`LinkedPlugin` —— 同一个 plugin.cpp 直接编进宿主时用它驱动，不链接 loader；与 `DynamicPlugin` 共用 `abi::PluginSession`（校验/会话/回滚/状态转移），所以两种编法行为逐字相同 |

### 4. kit 能力层（Tier 1，`nostos/kit/*`）

| 组件 | 目标 | 提供 |
|---|---|---|
| `kit/log.hpp` | header-only | 命名 logger、级别阈值、多 sink、异步有界队列（block 背压 / drop_oldest） |
| `kit/config.hpp` | header-only | 四层取值（默认←JSON←环境←覆盖）+ `on_change` 热更；JSON 用 vendored nlohmann/json |
| `kit/timer.hpp` | header-only | every / after / throttle / debounce，回调经 dispatch 投回宿主线程 |
| `kit/task.hpp` | header-only | 后台任务：block（join）/ discard（安全逃逸）两种关停语义 + 并发上限 |
| `kit/scratch.hpp` | header-only | 同步 emit 的载荷稳定缓冲：`Scratch::hold(string)→const char*`，作用域即寿命，地址跨后续 hold 稳定（事件载荷"调用期有效"纪律的调用点表达） |
| `kit/http.hpp` | `nostos::kit_http` | 单线程 select 循环 HTTP/1.1 + SSE：每客户端有界缓冲做背压，慢客户端不可能拖住别人 |
| `kit/win.hpp` | header-only | 文件选择对话框 + UTF-8 转换（非 Windows 降级） |
| `kit/process.hpp` | header-only | 子进程生命周期（Windows） |
| `abi/nostos_services.h` | header-only | 知名服务表的 C 布局（`nostos.log.v1` 等） |

kit 侧的"effect 记账"助手：`kit::Subscriptions`（VS Code Disposable 形态，逆序统一释放）。
依赖台账：vendored 依赖仅 `nlohmann/json` 3.11.3（header-only，MIT），随 kit 分发。

---

## 保证什么

这七条是它真正的价值，也是写代码时不能被破坏的约束：

1. **激活即事务**：`Host::start()` 里任一 `activate()` 抛异常 → 已激活的组件 + 失败者的半成品全部回滚；`start()` 可重试。
2. **取用即依赖**：依赖边在"取到服务"的那一刻记录，没有单独声明的运行期依赖图；撤销时**依赖者先行回滚**，任何时刻都不存在"活跃组件持有已消失服务"的边。
3. **逆序即安全**：服务必须先提供才能取用 ⇒ 声明序本身就是拓扑序 ⇒ 逆序回滚无需环检测、无需运行期图。
4. **回滚不会失败**：`dispose()` / `deactivate()` 必须 `noexcept`，违例 `std::terminate`（与"析构不得抛"同一纪律）。
5. **名称哈希即身份**：跨编译器稳定，`typeid` 只允许在同一 ABI 域内做类型安检。
6. **ABI 只追加**：`struct_size` 逐字段门控 + `abi_version` 标识契约 ⇒ 新宿主容忍老插件、老宿主读新插件；病态表在**加载期**被点名拒绝。
7. **诊断点名**：编译期靠"出错的实例化即文案"（`unsatisfied_requirement_error<组件, 服务, ...>`），运行期靠异常消息，永远指认具体组件/服务/插件路径。

---

## 不提供什么（明确不做，或属于 Phase 4）

- **不做**跨进程/网络插件分发、反射式配置装配、默认状态序列化、把物理卸载当正确性承诺。
- **还没有**：PENDING 自动重激活、ServiceSlot 热替换、文件监控热重载、quiescence/drain 在途计数、插件猝死观测（见设计文档 §7 的 Phase 4）。
- **单线程核心**：激活/卸载/派发都在一个 Executor 上；`Executor` 是留给将来的接缝（`Host::set_executor` / `Context::executor`），核心自身目前同步执行、不投递。跨线程投递要你自己实现加锁的 Executor；`SafeExecutor` 不是它 —— 它只给任务加**失败隔离**（任务抛异常时报告给 sink，而不是把泵打崩），见 `executor.hpp` 与 `nostos_test/test_executor.cpp`。
- **不含业务设施**：没有 UI、没有网络、没有日志实现 —— 只有一条 `LogSink` 接缝（`HostCore::set_log_sink`）。
- **插件能力有边界**：插件只能提供 published 表（原生 C++ 服务跨不了 ABI）；插件不是 `Host` 组件，因此没有编译期 Requires/Provides 检查，也不会自动重激活。

---

## 两分钟上手

### A. 组件式（同一进程、同一 ABI 域）

```cpp
#include <nostos/nostos.hpp>

struct Config {};
struct Server {
    using Requires = std::tuple<nostos::Svc<"app.config", Config>>;
    void activate(nostos::Context& ctx) {
        Config& cfg = ctx.require<"app.config", Config>();   // 取用即依赖
        ctx.scope().defer([] { /* 逆序回滚时执行 */ });        // 任何副作用都挂进 scope
        ctx.scope().own(start_something(cfg));                // 任何 RAII 都能被收养
    }
    void onStop() noexcept { /* 需要有序清理时用 */ }
};

nostos::Host<ConfigProvider, Server> host;
host.start();          // 事务：失败全量回滚
host.revoke<"app.config">();   // 运行期撤销：依赖者先回滚
host.shutdown();
```

### B. 插件式（跨编译器、运行期加载）

```cpp
// 插件：只包含 SDK 头，导出一个符号
#include <nostos/abi/plugin.hpp>
struct MyPlugin {
    static constexpr std::string_view name = "my.plugin";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(1, 0, 0);
    nostos_status activate(const nostos::abi::Host& host) {
        host.log(2, "activated");
        return NOSTOS_OK;
    }
    void deactivate() noexcept {}
};
NOSTOS_PLUGIN(MyPlugin)


## 许可

[MIT](LICENSE) —— 可以商用、闭源链接、修改后再发布；唯一义务是保留版权声明与许可文本。
插件可以用任何协议授权你的业务代码。
