/* nostos/abi/nostos_abi.h —— 冻结的 C ABI 契约（NOSTOS_ABI_VERSION 1）。
 *
 * 这是唯一跨越动态 plugin 边界的东西。plugin 内部可以随意用 C++；跨越此
 * 边界只传 C 标量、函数指针和 void*。
 *
 * 五条边界纪律（docs/design.md §4.7）：
 *   1. 谁分配谁释放 —— 宿主绝不释放 plugin 的内存，反之亦然。跨边界交出的
 *      缓冲区由其创建者持有。
 *   2. 异常绝不跨越此边界。回调实际上都是 noexcept；错误经 nostos_status
 *      返回码报告。
 *   3. 只有 C 类型跨边界。没有 C++ 对象、没有 shared_ptr、没有
 *      std::string、没有异常、没有 RTTI。
 *   4. 每个 struct 以 uint32_t struct_size + uint32_t abi_version 开头。
 *      读取方必须先检查两者再触碰其余字段；增长 struct 是向后兼容的变更。
 *   5. 跨边界的字符串与载荷只在调用期有效；要留存必须拷贝。
 *
 * 增长而不破坏（纪律 4 的实操）：
 *   新字段追加在 struct 的末尾，读取方在触碰它之前先按写入方的 struct_size
 *   门控。因此 NOSTOS_ABI_VERSION 始终是 1：它命名的是*契约*，struct_size
 *   命名的则是对端编译时所依据的*修订*。下面两种写法都正确，且刻意等价：
 *
 *     // 针对本头文件编译的读取方（直接点名新字段）
 *     if (NOSTOS_HOST_HAS(host, emit)) host->emit(id, payload);
 *
 *     // 针对更老的头文件编译的读取方（比较自己的大小）
 *     if (host->struct_size >= sizeof(nostos_host_api)) { ...v1 fields... }
 *
 *   既有字段绝不改变含义、绝不重排、绝不删除。NOSTOS_HOST_API_V1_SIZE 是
 *   冻结 v1 前缀的大小。
 *
 * v1 的已知缺口（刻意的，见 docs/design.md §4.7）：没有让 plugin *提供*
 * 原生（C++）服务的回调——只有 published 表服务；也没有办法观察到未先
 * deactivate 就死掉的 plugin。
 *
 * 服务与事件标识：对服务/事件名取的 fnv1a-64，与 C++ 核心所用
 * （nostos::svc_id / nostos::event_id）是同一个。id 在编译期计算，在任何
 * 编译器/ABI 域中都一致。
 */
#ifndef NOSTOS_ABI_H
#define NOSTOS_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOSTOS_ABI_VERSION 1u

/* 为导出标记 plugin 入口符号。Windows 上的 plugin 必须在包含本头文件之前
 * 定义 NOSTOS_BUILDING_PLUGIN；宿主从不使用它。 */
#if defined(_WIN32) && defined(NOSTOS_BUILDING_PLUGIN)
#  define NOSTOS_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define NOSTOS_EXPORT __attribute__((visibility("default")))
#else
#  define NOSTOS_EXPORT
#endif

typedef enum nostos_status {
    NOSTOS_OK = 0,
    NOSTOS_ERR_BAD_ABI = 1,   /* struct_size / abi_version 不匹配 */
    NOSTOS_ERR_BAD_STATE = 2, /* state 指针不可用 */
    NOSTOS_ERR_INVALID_ARG = 3,
    NOSTOS_ERR_OUT_OF_MEMORY = 4,
    NOSTOS_ERR_FAILED = 5     /* plugin 报告的通用失败 */
} nostos_status;

/* 绑定桥的投递器（poster）：插件在 activate 期间（此时本桥在 TLS 上）经
 * nostos_host_api::acquire_poster 领取，随激活会话失效——deactivate 之后
 * post 返回 NOSTOS_ERR_BAD_STATE，fn 不再执行。post 从任意线程调用都是
 * 安全的；宿主在宿主线程上执行 fn(arg)，执行期间以领取者的桥包裹，
 * 因此 fn 体内完整的宿主 API 可用。fn 约定不抛异常（宿主执行侧仍会兜底）。
 *
 * 所有权语义（完整版）：post 返回 OK 只代表"已入队"——**不保证执行**。
 * 会话作废时尚未排水的任务会被作废；投递方有两种出口：
 *   1) 注册 on_drop（见下）：作废时宿主逐条回调，投递方就地回收；
 *   2) 未注册：静默丢弃，载荷由投递方自理（即泄漏）。 */
typedef struct nostos_poster {
    uint32_t struct_size;   /* 由宿主填充 sizeof(nostos_poster) */
    void* self;             /* 宿主内部状态（借用；随桥存活） */
    nostos_status (*post)(void* self, void (*fn)(void* arg), void* arg);

    /* 尾部追加（NOSTOS_POSTER_HAS(poster, on_drop) 门控）：注册本 poster 的
     * "丢弃回调"。会话作废时仍未执行的已入队任务，宿主在**宿主线程**上
     * 逐条调用 fn(arg)（arg = 当初 post 的 arg），投递方据此回收载荷。
     * 约定：fn 必须是静态的、只触碰 arg——回调发生在会话作废之后，插件
     * 对象已析构（代码段仍映射到 unload 为止），不得触碰插件成员；fn
     * 不抛异常。同一 poster 反复注册以最后一次为准 ⇒ 同一 poster 应投递
     * 同构载荷（或 fn 自带类型标记）。未注册/旧宿主 ⇒ 静默丢弃。 */
    nostos_status (*on_drop)(void* self, void (*fn)(void* arg));
} nostos_poster;

#define NOSTOS_POSTER_HAS(poster, field)                                  \
    ((poster) != NULL &&                                                  \
     (poster)->struct_size >=                                             \
         (uint32_t)(offsetof(nostos_poster, field) + sizeof((poster)->field)))

/* published 服务的枚举条目（nostos_host_api::published_list 用）。
 * name 指向注册表内的 UTF-8 串：随该条目存活——unpublish / 重新发布后失效，
 * 要留存必须拷贝（纪律 5 的注册表版）。 */
typedef struct nostos_published_entry {
    uint64_t svc_id;
    const char* name;
} nostos_published_entry;

/* 由宿主注入 nostos_plugin_api::activate。表内的函数指针在宿主生命周期内
 * 有效。 */
typedef struct nostos_host_api {
    /* ---- 冻结的 v1 前缀：绝不重排、绝不改动 ------------------------------ */
    uint32_t struct_size; /* 写入方构建时的 sizeof(nostos_host_api) */
    uint32_t abi_version; /* 宿主构建时所用的 NOSTOS_ABI_VERSION */

    /* Published 服务：返回注册在 svc_id 名下的接口表，没有则返回 NULL。表的
     * 布局由该服务自己的共享接口头定义；宿主绝不检视它（纪律 3）。 */
    const void* (*service)(uint64_t svc_id);

    /* 事件：订阅存续期间，evt_id 每次 emit 都会执行 fn(user, payload)。
     * payload 是该事件的共享头 C struct，仅在回调内有效（纪律 5）。
     * 返回订阅 token，失败返回 0。 */
    uint32_t (*on)(uint64_t evt_id, void (*fn)(void* user, const void* payload), void* user);
    void (*off)(uint32_t subscription);

    /* 日志。level：0=trace 1=debug 2=info 3=warn 4=error。
     * msg 为 UTF-8，调用期有效（纪律 5）。 */
    void (*log)(int level, const char* msg);

    /* ---- v1 冻结之后追加：按 struct_size 门控 ---------------------------- */

    /* 在 svc_id 名下发布接口表，供其他 plugin 与宿主解析。表的所有权留在
     * plugin 手中，且必须存活到 unpublish（或 deactivate 返回——激活期间
     * 发布的一切宿主会自行撤销）。id 重复、或表的 header 缺失/无效时返回
     * NOSTOS_ERR_INVALID_ARG。 */
    nostos_status (*publish)(uint64_t svc_id, const void* table);
    void (*unpublish)(uint64_t svc_id);

    /* 向宿主的类型化总线对 evt_id emit 事件：原生监听者（把共享头 struct
     * 看作其 C++ 类型）与所有裸订阅者都会执行，按注册顺序。payload 为借用，
     * 必须在本次调用期间保持有效。 */
    void (*emit)(uint64_t evt_id, const void* payload);

    /* 取用绑定到调用方激活会话的投递器（poster）。在 activate 期间调用
     * （此时调用方的桥已安装）；用 NOSTOS_HOST_HAS(host, acquire_poster)
     * 门控。见上文 nostos_poster。out 为空返回 NOSTOS_ERR_INVALID_ARG，
     * 分配失败返回 NOSTOS_ERR_FAILED。 */
    nostos_status (*acquire_poster)(struct nostos_poster* out);

    /* 枚举 published 服务（尾部追加；用 NOSTOS_HOST_HAS(host, published_list)
     * 门控）。按注册顺序最多写 cap 个条目并返回写入数；cap == 0（或 out 为
     * NULL）只返回当前总数，不写入。这是跨 ABI 域的“发现”面——工具自述、
     * 状态面板之类；同一 ABI 域内的宿主直接用 PublishedRegistry::entries()。 */
    uint32_t (*published_list)(struct nostos_published_entry* out, uint32_t cap);
} nostos_host_api;

/* 冻结 v1 前缀的大小 —— 更老的对端 sizeof() 会得到的值。 */
#define NOSTOS_HOST_API_V1_SIZE ((uint32_t)offsetof(nostos_host_api, publish))

/* 当填充 *host 的宿主也填充了 `field` 时为真。只解引用写入方承诺已写过
 * 的字段。 */
#define NOSTOS_HOST_HAS(host, field)                                    \
    ((host) != NULL &&                                                  \
     (host)->struct_size >=                                             \
         (uint32_t)(offsetof(nostos_host_api, field) + sizeof((host)->field)))

/* 贡献的配置项（nostos_manifest::config 数组元素）。default_value 是字符串
 * 形态，数值型由使用方转换；summary 供面板/文档展示。 */
typedef struct nostos_config_contrib {
    const char* key;
    const char* default_value;
    const char* summary;
} nostos_config_contrib;

/* 插件的声明式 manifest（可为 NULL = 无声明；三个指针也都可以是 NULL）。
 * 语义由宿主策略实现（docs/proposal-kit.md §6.3）：
 *   inject  —— 依赖的服务名；宿主可在依赖未就绪时不激活（懒激活）。
 *   provide —— 声明本插件提供的服务名；宿主可在 activate 返回后做
 *              声明↔实际校验（软告警，不失败）。
 *   config  —— 贡献的配置项与默认值。 */
typedef struct nostos_manifest {
    uint32_t struct_size;                 /* 写入方填充 sizeof(nostos_manifest) */
    const char* const* inject;            /* NULL 结尾 */
    const char* const* provide;           /* NULL 结尾 */
    const nostos_config_contrib* config;  /* NULL 结尾 */
} nostos_manifest;

/* manifest 的字段门控（与 NOSTOS_HOST_HAS 同一机制）。 */
#define NOSTOS_PLUGIN_HAS(plugin, field)                                  \
    ((plugin) != NULL &&                                                  \
     (plugin)->struct_size >=                                             \
         (uint32_t)(offsetof(nostos_plugin_api, field) + sizeof((plugin)->field)))

/* 每个 plugin 经 nostos_plugin_entry() 导出的函数表。
 * 被指向的表必须在 plugin 的已加载生命周期内保持有效。 */
typedef struct nostos_plugin_api {
    uint32_t struct_size; /* 写入方填充 sizeof(nostos_plugin_api) */
    uint32_t abi_version; /* plugin 支持的最高 NOSTOS_ABI_VERSION */

    const char* name;    /* 静态存储期，plugin 生命周期内有效（纪律 5） */
    uint32_t version;    /* plugin 版本，打包为 (major<<24|minor<<16|patch) */

    /* 把 plugin 装配起来。失败时返回非 OK；宿主保证不再有后续调用，且失败的
     * activate 不留残渣——plugin 报告失败之后不得再要求自行清理。成功时
     * *out_state 会被原样交回给其余每个调用；失败时其值未指定、宿主忽略
     * （loader 改为清除自己的指针——见 DynamicPlugin::activate）。 */
    nostos_status (*activate)(const nostos_host_api* host, void** out_state);

    /* 完全释放 activate() 创建的一切。每次成功的 activate 恰好调用一次；
     * 此后 plugin 仍保持已加载。 */
    nostos_status (*deactivate)(void* state);

    /* 热重载的可选状态转移；两者皆可为 NULL。返回 NOSTOS_ERR_* 表示
     * “状态不可转移”——宿主改走冷重新激活。 */
    nostos_status (*save_state)(void* state, void* buf, size_t cap, size_t* out_len);
    nostos_status (*load_state)(void* state, const void* buf, size_t len);

    /* 声明式 manifest（可选，可为 NULL）：inject / provide / 贡献配置项。
     * 宿主用它做懒激活、依赖记账与配置发现（docs/proposal-kit.md §6.3）。 */
    const nostos_manifest* manifest;
} nostos_plugin_api;

/* 宿主可读的最小 plugin 表。struct_size 更小的 plugin 会被宿主拒绝
 * （NOSTOS_ERR_BAD_ABI）：必需字段会落在 plugin 实际填充的区域之外。新字段
 * 追加在 load_state 之后，届时本常量变为
 * offsetof(nostos_plugin_api, <首个追加字段>) 并不再移动。 */
#define NOSTOS_PLUGIN_API_V1_SIZE ((uint32_t)offsetof(nostos_plugin_api, manifest))

/* 每个 nostos plugin 都导出的唯一符号。 */
NOSTOS_EXPORT const nostos_plugin_api* nostos_plugin_entry(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NOSTOS_ABI_H */
