/* nostos/abi/nostos_abi.h — the frozen C ABI contract (NOSTOS_ABI_VERSION 1).
 *
 * The ONLY thing that crosses the dynamic plugin boundary. Plugins may use
 * any C++ they like internally; across this boundary pass C scalars, function
 * pointers and void* only.
 *
 * The five boundary disciplines (docs/design.md §4.7):
 *   1. Who allocates frees — the host never frees plugin memory and vice
 *      versa. Buffers handed across the boundary are owned by their creator.
 *   2. Exceptions never cross this boundary. Callbacks are effectively
 *      noexcept; errors are reported through nostos_status return codes.
 *   3. Only C types cross the boundary. No C++ objects, no shared_ptr, no
 *      std::string, no exceptions, no RTTI.
 *   4. Every struct starts with uint32_t struct_size + uint32_t abi_version.
 *      Readers must check both before touching further fields; growing a
 *      struct is a backward-compatible change.
 *   5. Strings and payloads crossing the boundary are valid only for the
 *      duration of the call; retain by copying.
 *
 * Growing without breaking (discipline 4 in practice):
 *   A new field is appended at the END of a struct and readers gate on the
 *   writer's struct_size before touching it. NOSTOS_ABI_VERSION therefore
 *   stays 1: it names the *contract*, while struct_size names the *revision*
 *   the peer was compiled against. Both idioms below are correct and
 *   deliberately equivalent:
 *
 *     // a reader compiled against this header (names the new field)
 *     if (NOSTOS_HOST_HAS(host, emit)) host->emit(id, payload);
 *
 *     // a reader compiled against an older header (compares its own size)
 *     if (host->struct_size >= sizeof(nostos_host_api)) { ...v1 fields... }
 *
 *   Existing fields never change meaning, are never reordered and are never
 *   removed. NOSTOS_HOST_API_V1_SIZE is the size of the frozen v1 prefix.
 *
 * Known v1 gaps (deliberate, see docs/design.md §4.7): there is no callback
 * for a plugin to *provide* a native (C++) service — only published table
 * services — and no way to observe a plugin that dies without deactivating.
 *
 * Service and event identity: the same fnv1a-64 of the service/event name
 * used by the C++ core (nostos::svc_id / nostos::event_id). Ids are computed
 * at compile time and are identical in every compiler/ABI domain.
 */
#ifndef NOSTOS_ABI_H
#define NOSTOS_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOSTOS_ABI_VERSION 1u

/* Marks the plugin entry symbol for export. Plugins on Windows must define
 * NOSTOS_BUILDING_PLUGIN before including this header; hosts never use it. */
#if defined(_WIN32) && defined(NOSTOS_BUILDING_PLUGIN)
#  define NOSTOS_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define NOSTOS_EXPORT __attribute__((visibility("default")))
#else
#  define NOSTOS_EXPORT
#endif

typedef enum nostos_status {
    NOSTOS_OK = 0,
    NOSTOS_ERR_BAD_ABI = 1,   /* struct_size / abi_version mismatch */
    NOSTOS_ERR_BAD_STATE = 2, /* state pointer unusable */
    NOSTOS_ERR_INVALID_ARG = 3,
    NOSTOS_ERR_OUT_OF_MEMORY = 4,
    NOSTOS_ERR_FAILED = 5     /* plugin-reported generic failure */
} nostos_status;

/* 绑定桥的投递器（poster）：插件在 activate 期间（此时本桥在 TLS 上）经
 * nostos_host_api::acquire_poster 领取，随激活会话失效——deactivate 之后
 * post 返回 NOSTOS_ERR_BAD_STATE，fn 不再执行。post 从任意线程调用都是
 * 安全的；宿主在宿主线程上执行 fn(arg)，执行期间以领取者的桥包裹，
 * 因此 fn 体内完整的宿主 API 可用。fn 约定不抛异常（宿主执行侧仍会兜底）。 */
typedef struct nostos_poster {
    uint32_t struct_size;   /* 由宿主填充 sizeof(nostos_poster) */
    void* self;             /* 宿主内部状态（借用；随桥存活） */
    nostos_status (*post)(void* self, void (*fn)(void* arg), void* arg);
} nostos_poster;

/* Injected by the host into nostos_plugin_api::activate. Function pointers
 * in this table stay valid for the host's lifetime. */
typedef struct nostos_host_api {
    /* ---- frozen v1 prefix: never reordered, never changed ---------------- */
    uint32_t struct_size; /* sizeof(nostos_host_api) as built by the writer */
    uint32_t abi_version; /* NOSTOS_ABI_VERSION the host was built with */

    /* Published services: returns the interface table registered under
     * svc_id, or NULL. The table layout is defined by the service's own
     * shared interface header; the host never inspects it (discipline 3). */
    const void* (*service)(uint64_t svc_id);

    /* Events: fn(user, payload) runs for each emit of evt_id while the
     * subscription lives. payload is the event's shared-header C struct,
     * valid for the duration of the callback only (discipline 5).
     * Returns a subscription token, 0 on failure. */
    uint32_t (*on)(uint64_t evt_id, void (*fn)(void* user, const void* payload), void* user);
    void (*off)(uint32_t subscription);

    /* Logging. level: 0=trace 1=debug 2=info 3=warn 4=error.
     * msg is UTF-8, valid for the duration of the call (discipline 5). */
    void (*log)(int level, const char* msg);

    /* ---- appended after the v1 freeze: gate on struct_size -------------- */

    /* Publish an interface table under svc_id so that other plugins and the
     * host can resolve it. The plugin keeps ownership of the table and must
     * keep it alive until unpublish (or until deactivate returns — the host
     * revokes everything published during an activation on its own).
     * Returns NOSTOS_ERR_INVALID_ARG for a duplicate id or a table whose
     * header is missing/invalid. */
    nostos_status (*publish)(uint64_t svc_id, const void* table);
    void (*unpublish)(uint64_t svc_id);

    /* Emit an event into the host's typed bus for evt_id: native listeners
     * (which see the shared-header struct as its C++ type) plus every raw
     * subscriber run, in registration order. payload is borrowed and must
     * stay valid for the duration of this call. */
    void (*emit)(uint64_t evt_id, const void* payload);

    /* Acquire a poster bound to the caller's activation session. Call during
     * activate (the caller's bridge is installed); gate with
     * NOSTOS_HOST_HAS(host, acquire_poster). See nostos_poster above.
     * Returns NOSTOS_ERR_INVALID_ARG for a null out, NOSTOS_ERR_FAILED on
     * allocation failure. */
    nostos_status (*acquire_poster)(struct nostos_poster* out);
} nostos_host_api;

/* Size of the frozen v1 prefix — what an older peer's sizeof() would be. */
#define NOSTOS_HOST_API_V1_SIZE ((uint32_t)offsetof(nostos_host_api, publish))

/* True when the host that filled *host also filled `field`. Only ever
 * dereferences fields the writer promised to have written. */
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
    uint32_t struct_size;                 /* sizeof(nostos_manifest) */
    const char* const* inject;            /* NULL 结尾 */
    const char* const* provide;           /* NULL 结尾 */
    const nostos_config_contrib* config;  /* NULL 结尾 */
} nostos_manifest;

/* manifest 的字段门控（与 NOSTOS_HOST_HAS 同一机制）。 */
#define NOSTOS_PLUGIN_HAS(plugin, field)                                  \
    ((plugin) != NULL &&                                                  \
     (plugin)->struct_size >=                                             \
         (uint32_t)(offsetof(nostos_plugin_api, field) + sizeof((plugin)->field)))

/* The function table every plugin exports through nostos_plugin_entry().
 * The pointed-to table must remain valid for the plugin's loaded lifetime. */
typedef struct nostos_plugin_api {
    uint32_t struct_size; /* sizeof(nostos_plugin_api) */
    uint32_t abi_version; /* highest NOSTOS_ABI_VERSION the plugin supports */

    const char* name;    /* static, plugin-lifetime (discipline 5) */
    uint32_t version;    /* plugin version, packed (major<<24|minor<<16|patch) */

    /* Wire the plugin up. On failure return non-OK; the host guarantees no
     * further calls and that a failing activate leaves no residue — the
     * plugin must not require its own cleanup after reporting failure.
     * On success *out_state is handed back verbatim to every other call; on
     * failure its value is unspecified and the host ignores it (the loader
     * clears its own pointer instead — see DynamicPlugin::activate). */
    nostos_status (*activate)(const nostos_host_api* host, void** out_state);

    /* Fully release everything activate() created. Called exactly once per
     * successful activate; the plugin stays loaded afterwards. */
    nostos_status (*deactivate)(void* state);

    /* Optional state transfer for hot reload; either may be NULL. Returning
     * NOSTOS_ERR_* means "state not transferable" — the host proceeds with a
     * cold reactivation. */
    nostos_status (*save_state)(void* state, void* buf, size_t cap, size_t* out_len);
    nostos_status (*load_state)(void* state, const void* buf, size_t len);

    /* 声明式 manifest（可选，可为 NULL）：inject / provide / 贡献配置项。
     * 宿主用它做懒激活、依赖记账与配置发现（docs/proposal-kit.md §6.3）。 */
    const nostos_manifest* manifest;
} nostos_plugin_api;

/* Smallest plugin table a host may read. A host refuses (NOSTOS_ERR_BAD_ABI)
 * any plugin reporting a smaller struct_size: the required fields would be
 * outside the region the plugin actually filled. New fields get appended
 * after load_state, at which point this constant becomes
 * offsetof(nostos_plugin_api, <first appended field>) and stops moving. */
#define NOSTOS_PLUGIN_API_V1_SIZE ((uint32_t)offsetof(nostos_plugin_api, manifest))

/* The one symbol every nostos plugin exports. */
NOSTOS_EXPORT const nostos_plugin_api* nostos_plugin_entry(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NOSTOS_ABI_H */
