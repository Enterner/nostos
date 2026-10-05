#pragma once
// nostos L0 —— 错误词汇表。
//
// 库抛出的每个错误都派生自 nostos_error，host 用一个 catch 子句就能接住整个
// 家族，同时仍能点名具体是哪种失败。消息在抛出时构造，并且总是指出肇事实体
// （service、component、event、插件路径）——本库的诊断规则对运行期错误与
// 编译期错误一视同仁（见 di.hpp）。
//
// 每个错误还以成员加访问器的形式携带自己的标识数据，host 可以程序化地处理
// 失败，而不必解析 what()。这对既定规划中的工作很重要，比如 Phase 4 的
// PENDING 自动再激活：host 必须知道自己在等的是*哪个* service id，才能注册
// waiter。人读 what()，程序读访问器。
//
// NOTE: 本头文件中的任何东西都不得把异常抛出动态插件边界。跨 C ABI 的失败
// 一律经 nostos_status 上报（nostos_abi.h，纪律 2）；这些类型供 host 侧与
// 同一 ABI 域内的代码使用。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>

namespace nostos {

class nostos_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

inline std::string hex64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

// 所有"具名 service + id"的消息都共享同一个形状：
//   <prefix><name>' (<id>)<suffix>
// 收拢在一处，文案就不会在各错误类型之间漂移（对它做断言的测试也因此一直
// 有效）。
inline std::string quoted_id_message(std::string_view prefix, std::string_view name,
                                     std::uint64_t id, std::string_view suffix) {
    return std::string(prefix) + std::string(name) + "' (" + hex64(id) + ")" + std::string(suffix);
}

}  // namespace detail

class missing_service : public nostos_error {
public:
    missing_service(std::string_view name, std::uint64_t id)
        : nostos_error(detail::quoted_id_message("nostos: no service provides '", name, id, "")),
          name_(name),
          id_(id) {}

    // host 注册 waiter（Phase 4 PENDING）或按 service 路由失败所需的东西：
    // 与 registry 的键完全相同的 id。
    std::string_view service_name() const noexcept { return name_; }
    std::uint64_t service_id() const noexcept { return id_; }

private:
    std::string name_;  // 自有拷贝：调用方递过来的可能是临时对象的 view
    std::uint64_t id_ = 0;
};

class duplicate_service : public nostos_error {
public:
    duplicate_service(std::string_view name, std::uint64_t id)
        : nostos_error(detail::quoted_id_message("nostos: service '", name, id,
                                                 " is already provided")),
          name_(name),
          id_(id) {}

    std::string_view service_name() const noexcept { return name_; }
    std::uint64_t service_id() const noexcept { return id_; }

private:
    std::string name_;
    std::uint64_t id_ = 0;
};

class type_mismatch : public nostos_error {
public:
    type_mismatch(std::string_view name, const std::type_info& requested,
                  const std::type_info& registered)
        : nostos_error("nostos: service '" + std::string(name) + "' is registered as '" +
                       registered.name() + "' but requested as '" + requested.name() + "'"),
          name_(name),
          requested_(requested.name()),
          registered_(registered.name()) {}

    std::string_view service_name() const noexcept { return name_; }

    // NOTE: 这些是 std::type_info::name() 的拼写——实现定义（MSVC 打印
    // "struct A"，GCC 打印修饰名）。只在同一个 ABI 域内有意义，与抛出该错误
    // 的检查完全相同。
    std::string_view requested_type() const noexcept { return requested_; }
    std::string_view registered_type() const noexcept { return registered_; }

private:
    std::string name_;
    std::string_view requested_;   // type_info::name() 具有静态生存期
    std::string_view registered_;
};

// 声明了 Provides（见 di.hpp）的 component 注册了一个其声明未列出的 service：
// 声明的契约与实现脱节了。编译期检查看不见这件事，因此在注册期强制。
class undeclared_provide : public nostos_error {
public:
    undeclared_provide(std::string_view name, std::size_t component)
        : nostos_error("nostos: component #" + std::to_string(component) + " provides '" +
                       std::string(name) + "' but its Provides declaration does not list it"),
          name_(name),
          component_(component) {}

    std::string_view service_name() const noexcept { return name_; }
    std::size_t component_index() const noexcept { return component_; }

private:
    std::string name_;
    std::size_t component_ = 0;
};

// component 取用了一个其 Requires 声明完全未列出该 id 的 service。
// undeclared_provide 的镜像：声明的契约与实现脱节，而编译期检查看不见调用点。
class undeclared_require : public nostos_error {
public:
    undeclared_require(std::string_view name, std::size_t component)
        : nostos_error("nostos: component #" + std::to_string(component) + " requires '" +
                       std::string(name) + "' but its Requires declaration does not list it"),
          name_(name),
          component_(component) {}

    std::string_view service_name() const noexcept { return name_; }
    std::size_t component_index() const noexcept { return component_; }

private:
    std::string name_;
    std::size_t component_ = 0;
};

// component 确实声明了这个 id，但声明的 C++ 类型与调用点要求的不同——
// `Requires = Svc<"x", A>` 而调用的是 `service<"x", B>()`。di.hpp 检查的是
// 声明与声明之间；能把声明与调用点对起来的只有这一处检查。没有它，release
// 构建里这种不匹配会无声地重解释内存。
class declared_type_mismatch : public nostos_error {
public:
    declared_type_mismatch(std::string_view name, std::size_t component,
                           const std::type_info& declared, const std::type_info& requested)
        : nostos_error("nostos: component #" + std::to_string(component) + " requires '" +
                       std::string(name) + "' as '" + requested.name() +
                       "' but its Requires declaration lists '" + declared.name() + "'"),
          name_(name),
          component_(component),
          declared_(declared.name()),
          requested_(requested.name()) {}

    std::string_view service_name() const noexcept { return name_; }
    std::size_t component_index() const noexcept { return component_; }

    // type_info::name() 的拼写（实现定义），只在同一个 ABI 域内有意义——
    // 注意事项同 type_mismatch。
    std::string_view declared_type() const noexcept { return declared_; }
    std::string_view requested_type() const noexcept { return requested_; }

private:
    std::string name_;
    std::size_t component_ = 0;
    std::string_view declared_;
    std::string_view requested_;
};

// published table 在注册期未通过校验。registry 直接拒收未盖章的表，因此这是
// 边界上的硬性契约违约，不是可恢复状态——但它仍然指名错在何处并携带肇事值，
// host 上报时无需做字符串匹配。
class bad_published_table : public nostos_error {
public:
    enum class Reason {
        null_table,   // 压根没有表指针
        struct_size,  // struct_size 连 TableHeader 都盖不住
        abi_version,  // abi_version == 0：表从未被盖章
    };

    bad_published_table(std::string_view name, Reason reason, std::uint32_t value = 0)
        : nostos_error(message(name, reason, value)),
          name_(name),
          reason_(reason),
          value_(value) {}

    std::string_view service_name() const noexcept { return name_; }
    Reason reason() const noexcept { return reason_; }
    // Reason::struct_size 时为 struct_size，Reason::abi_version 时为
    // abi_version，null_table 时为 0。
    std::uint32_t offending_value() const noexcept { return value_; }

private:
    static std::string message(std::string_view name, Reason reason, std::uint32_t value) {
        switch (reason) {
            case Reason::null_table:
                return "nostos: published service '" + std::string(name) +
                       "' has no interface table";
            case Reason::struct_size:
                return "nostos: published service '" + std::string(name) +
                       "' has an unstamped table (struct_size=" + std::to_string(value) + ")";
            case Reason::abi_version:
                return "nostos: published service '" + std::string(name) +
                       "' declares abi_version 0";
        }
        return "nostos: published service '" + std::string(name) + "' is invalid";
    }

    std::string name_;
    Reason reason_ = Reason::null_table;
    std::uint32_t value_ = 0;
};

// 不按顺序调用 Host 入口（start() 两次、start() 之前 activate()、对存活的
// component 再次激活）。这是对 host 自身 API 的误用，不是 component 的失败
// ——因此它不经过 component 的 scope 回滚。调用涉及某个 component 时会带上
// 其索引。
class host_state_error : public nostos_error {
public:
    enum class Reason {
        already_started,  // 对已完全激活的 host 调 start()
        not_started,      // host 未启动就调 activate(index)
        already_active,   // 对存活的 component 调 activate(index)
    };

    static constexpr std::size_t no_component = static_cast<std::size_t>(-1);

    explicit host_state_error(Reason reason, std::size_t component = no_component)
        : nostos_error(message(reason)), reason_(reason), component_(component) {}

    Reason reason() const noexcept { return reason_; }
    std::size_t component_index() const noexcept { return component_; }

private:
    static std::string message(Reason reason) {
        switch (reason) {
            case Reason::already_started:
                return "nostos: host is already started";
            case Reason::not_started:
                return "nostos: activate(index) needs a started host";
            case Reason::already_active:
                return "nostos: component is already active";
        }
        return "nostos: host is in the wrong state";
    }

    Reason reason_ = Reason::not_started;
    std::size_t component_ = no_component;
};

// 两个不同的载荷类型共用一个 event 名。raw 通道按名字哈希作键，把插件的载荷
// 派发进错误的 bus 会重解释 C++ 类型；宁可拒绝，也不损坏内存。
class event_name_collision : public nostos_error {
public:
    event_name_collision(std::string_view name, std::uint64_t id)
        : nostos_error(detail::quoted_id_message("nostos: event name '", name, id,
                                                 " is used by two different payload types")),
          name_(name),
          id_(id) {}

    std::string_view event_name() const noexcept { return name_; }
    // 不命名为 event_id()：那会在本类（以及任何派生类）内遮蔽
    // nostos::event_id<E>。
    std::uint64_t event_id_value() const noexcept { return id_; }

private:
    std::string name_;
    std::uint64_t id_ = 0;
};

// 加载、校验或驱动动态插件失败。消息携带 OS 错误文本或 ABI 不匹配的说明，
// 绝不是裸状态码。
class plugin_error : public nostos_error {
public:
    using nostos_error::nostos_error;
};

}  // namespace nostos
