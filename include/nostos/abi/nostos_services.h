/* nostos/abi/nostos_services.h —— 知名服务表的 C 布局（Tier 0 冻结面）。
 *
 * 这里只给出表的 C 布局契约，svc 名以 "nostos." 前缀保留给第一方；实例由宿主侧
 * 包装发布（例如把 kit::Log 接成 nostos.log.v1）——内核与 kit 当前不含适配代码。
 * 布局走同样的纪律：字段只在尾部追加，读者按 struct_size 门控
 * （NOSTOS_PLUGIN_HAS / NOSTOS_HOST_HAS 的同款机制）。
 */
#ifndef NOSTOS_SERVICES_H
#define NOSTOS_SERVICES_H

#include "nostos_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* nostos.log.v1 —— kit 日志服务。level: 0=trace 1=debug 2=info 3=warn 4=error；
 * text 为 UTF-8、只在调用期有效。 */
typedef struct nostos_log_v1 {
    uint32_t struct_size;   /* 写入方填充 sizeof(nostos_log_v1) */
    uint32_t abi_version;   /* 1 */
    void* self;             /* 宿主内部状态（借用；随桥存活） */
    void (*say)(void* self, int level, const char* text);
} nostos_log_v1;

#define NOSTOS_LOG_V1_SIZE ((uint32_t)sizeof(nostos_log_v1))

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif /* NOSTOS_SERVICES_H */
