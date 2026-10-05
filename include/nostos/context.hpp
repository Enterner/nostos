#pragma once
// nostos L1 — Context（激活期 API）与 HostCore（Host 坐落于其上的非模板
// 机械部分）。
//
// Context 会被递交给 Component::activate()。它的形态编码了设计的两条核心
// 不变量：
//
// * 取用即依赖 — service<T>() 在取用发生的那一刻记录依赖边；不存在另外
//   的图声明。
// * 一切副作用经 scope() — 组件创建的每个可逆副作用都归其 Scope 所有，
//   因此 activate() 异常退出时组件会被自动回滚（激活 = 事务）。

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include "nostos/event.hpp"
#include "nostos/executor.hpp"
#include "nostos/guard.hpp"
#include "nostos/published.hpp"
#include "nostos/registry.hpp"
#include "nostos/scope.hpp"
#include "nostos/svc_id.hpp"

namespace nostos {

class HostCore {
public:
    HostCore() = default;
    HostCore(const HostCore&) = delete;
    HostCore& operator=(const HostCore&) = delete;

    ServiceRegistry& services() noexcept { return services_; }
    EventHub& events() noexcept { return events_; }
    Scope& root_scope() noexcept { return root_; }

    Executor& executor() noexcept { return *executor_; }
    void set_executor(Executor& e) noexcept { executor_ = &e; }  // 存活由调用方保证

    struct DependencyEdge {
        std::size_t consumer;  // 取用方组件的激活序号
        std::uint64_t service_id;
        std::string service_name;
    };

    // 记录当前由哪个组件提供哪个 service。registry 本身以 svc_id 为键，
    // 对组件一无所知；正是这张表使「卸载 X 的提供者」成为可能
    // （见 Host::revoke）。
    struct ProvideRecord {
        std::size_t provider;  // 提供方组件的激活序号
        std::uint64_t service_id;
        std::string service_name;
    };

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    void record_dependency(std::size_t consumer, std::uint64_t id, std::string name) {
        edges_.push_back(DependencyEdge{consumer, id, std::move(name)});
    }

    void clear_dependencies_for(std::size_t consumer) {
        edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                    [&](const DependencyEdge& e) { return e.consumer == consumer; }),
                     edges_.end());
    }

    const std::vector<DependencyEdge>& dependency_edges() const noexcept { return edges_; }

    void record_provide(std::size_t provider, std::uint64_t id, std::string name) {
        provides_.push_back(ProvideRecord{provider, id, std::move(name)});
    }

    // 只移除注册记录本身；service 的擦除由提供方组件的 scope 负责。
    void clear_provide(std::size_t provider, std::uint64_t id) {
        provides_.erase(std::remove_if(provides_.begin(), provides_.end(),
                                       [&](const ProvideRecord& r) {
                                           return r.provider == provider && r.service_id == id;
                                       }),
                        provides_.end());
    }

    void clear_provides_for(std::size_t provider) {
        provides_.erase(std::remove_if(provides_.begin(), provides_.end(),
                                       [&](const ProvideRecord& r) { return r.provider == provider; }),
                        provides_.end());
    }

    std::size_t provider_of(std::uint64_t id) const noexcept {
        for (const auto& r : provides_)
            if (r.service_id == id) return r.provider;
        return npos;
    }

    const std::vector<ProvideRecord>& provides() const noexcept { return provides_; }

    // ---- published services（C ABI 表面，见 published.hpp） --------

    PublishedRegistry& published() noexcept { return published_; }
    const PublishedRegistry& published() const noexcept { return published_; }

    // ---- 日志 ----------------------------------------------------------

    // nostos_host_api::log 的落点。用 std::function 而非接口：ABI 层是在
    // 外来调用栈上调用它的，不应关心 sink 由谁持有。绝不能让它把异常抛出
    // ABI thunk。
    using LogSink = std::function<void(int level, std::string_view msg)>;

    void set_log_sink(LogSink sink) { log_sink_ = sink ? std::move(sink) : default_log_sink(); }
    const LogSink& log_sink() const noexcept { return log_sink_; }

    void log(int level, std::string_view msg) const {
        if (log_sink_) log_sink_(level, msg);
    }

    // 向 stderr 打印 "[nostos][warn] msg" — 宿主的最后兜底 sink
    // （插件在加载失败期间写日志时也仍有去处）。
    static LogSink default_log_sink() {
        return [](int level, std::string_view msg) {
            static const char* names[] = {"trace", "debug", "info", "warn", "error"};
            const char* name = (level >= 0 && level <= 4) ? names[level] : "?";
            std::fprintf(stderr, "[nostos][%s] %.*s\n", name, static_cast<int>(msg.size()),
                         msg.data());
        };
    }

    // ---- declared Provides（di.hpp） --------------------------------------

    void declare_provides(std::size_t component, std::vector<std::uint64_t> ids) {
        if (declared_provides_.size() <= component) declared_provides_.resize(component + 1);
        declared_provides_[component] = std::move(ids);
    }

    // 本组件是否允许注册 id：未声明 Provides（或声明为空列表）的组件
    // 不受约束。
    bool provide_is_declared(std::size_t component, std::uint64_t id) const noexcept {
        if (component >= declared_provides_.size()) return true;
        const auto& ids = declared_provides_[component];
        if (ids.empty()) return true;
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    // ---- declared Requires（di.hpp） --------------------------------------
    //
    // 取用方比提供方多需要一样东西：*类型*。`Requires = Svc<"x", A>` 与
    // 调用点上的 `service<"x", B>()` 是两段互不相干的文本，而 di.hpp 只在
    // 声明与声明之间做比较。若此处没有感知类型的检查，这种不匹配在
    // release 构建中将不可见（仅 Debug 生效的 registry 检查是唯一的其他
    // 防线，在 NDEBUG 下不会触发）。

    struct DeclaredRequire {
        std::uint64_t service_id;
        const std::type_info* type;
    };

    enum class RequireStatus {
        unconstrained,  // 组件未声明 Requires — 任何取用都放行
        matched,        // 已声明，且 C++ 类型与请求的完全一致
        not_listed,     // 声明了非空列表，但不含该 id
        type_conflict,  // 声明了该 id，但对应的 C++ 类型不同
    };

    struct RequireCheck {
        RequireStatus status = RequireStatus::unconstrained;
        const std::type_info* declared = nullptr;  // 仅在 type_conflict 时置位
    };

    void declare_requires(std::size_t component, std::vector<DeclaredRequire> list) {
        if (declared_requires_.size() <= component) declared_requires_.resize(component + 1);
        declared_requires_[component] = std::move(list);
    }

    RequireCheck check_require(std::size_t component, std::uint64_t id,
                               const std::type_info& requested) const noexcept {
        if (component >= declared_requires_.size()) return {RequireStatus::unconstrained, nullptr};
        const auto& list = declared_requires_[component];
        if (list.empty()) return {RequireStatus::unconstrained, nullptr};
        for (const auto& entry : list) {
            if (entry.service_id != id) continue;
            if (entry.type != nullptr && *entry.type == requested)
                return {RequireStatus::matched, entry.type};
            return {RequireStatus::type_conflict, entry.type};
        }
        return {RequireStatus::not_listed, nullptr};
    }

private:
    ServiceRegistry services_;
    EventHub events_;
    PublishedRegistry published_;
    Scope root_;
    InlineExecutor default_executor_;
    Executor* executor_ = &default_executor_;
    std::vector<DependencyEdge> edges_;
    std::vector<ProvideRecord> provides_;
    std::vector<std::vector<std::uint64_t>> declared_provides_;
    std::vector<std::vector<DeclaredRequire>> declared_requires_;
    LogSink log_sink_ = default_log_sink();
};

class Context {
public:
    Context(HostCore& host, std::size_t component_index, Scope& scope) noexcept
        : host_(&host), index_(component_index), scope_(&scope) {}

    // 本组件副作用所归属的 scope。凡在这里登记创建的东西（guard、defer、
    // 子 scope）都会随组件一起 unwind — 顺序有保证，无需手动清理。
    Scope& scope() const noexcept { return *scope_; }
    std::size_t component_index() const noexcept { return index_; }

    // ---- services --------------------------------------------------------

    // 提供一个归属本组件 scope 的 service：此刻注册，scope unwind 时自动
    // 注销。注销动作是在*这里*登记的，因此它在 LIFO 顺序中的位置于这一刻
    // 即被固定：组件在此调用之后 defer 的清理，会在 service 消失之前运行
    // — 这正是常见写法（先 provide，后 defer）。而*先于* provide 登记
    // 的 defer 会在注销之后运行，即彼时 service 已不存在。声明了
    // Provides 的组件只能注册其声明过的内容（见 di.hpp）。
    //
    // 生命周期：返回的是*裸* T&，不是保活句柄。它仅在本组件保持激活期间
    // 有效；必须在 revoke 之后继续存活的消费方，应持有 service()/require()
    // 返回的 ServiceHandle，由它使对象保活。裸 T& 做不到这一点。
    // 踩过: host.revoke<"id">() 之后旧引用读到的已是释放内存
    // （0xDDDDDDDD — MSVC 调试堆的填充值 — 且没有任何工具能诊断）。
    template <fixed_string Name, typename T, typename... Args>
    T& provide(Args&&... args) {
        constexpr std::uint64_t id = svc_id<Name>;
        if (!host_->provide_is_declared(index_, id))
            throw undeclared_provide(Name.view(), index_);
        T& ref = host_->services().template provide<Name, T>(std::forward<Args>(args)...);
        const std::size_t provider = index_;
        host_->record_provide(provider, id, std::string(Name.view()));
        scope_->defer([host = host_, provider, id] {
            host->services().erase(id);
            host->clear_provide(provider, id);
        });
        return ref;
    }

    // 取用一个 service。记录一条依赖边（取用即依赖），并返回共享句柄；
    // 即使 service 之后被 revoke，句柄仍然安全。没有任何组件提供 Name
    // 时抛出 missing_service。
    //
    // 声明的 Requires 列表在查找*之前*检查，因此偏离声明的调用点会被按
    // 本来面目报告（见 undeclared_require / declared_type_mismatch），而
    // 不是被误报成 service 缺失或类型不符。未声明 Requires 的组件不受
    // 约束，与 provide_is_declared 一致。
    template <fixed_string Name, typename T>
    ServiceHandle<T> service() const {
        constexpr std::uint64_t id = svc_id<Name>;
        const auto check = host_->check_require(index_, id, typeid(T));
        if (check.status == HostCore::RequireStatus::not_listed)
            throw undeclared_require(Name.view(), index_);
        if (check.status == HostCore::RequireStatus::type_conflict)
            throw declared_type_mismatch(Name.view(), index_, *check.declared, typeid(T));
        auto ptr = host_->services().template find<Name, T>();
        if (!ptr) throw missing_service(Name.view(), id);
        host_->record_dependency(index_, id, std::string(Name.view()));
        return ServiceHandle<T>(std::move(ptr));
    }

    // 借用（borrowed）访问 — 依赖记录相同，语义为引用。
    template <fixed_string Name, typename T>
    T& require() const {
        return *this->template service<Name, T>();
    }

    // ---- published services（C 接口表，见 published.hpp） -------

    // 在 Name 下发布一张 C 接口表，存活期为本组件的生命周期：可从任何
    // ABI 域经 nostos_host_api::service 解析，组件 scope unwind 时自动
    // 撤销。表必须经 abi::init_table 打标，且必须比 scope 活得久
    // （静态对象或成员，绝不能是临时对象）。declared-Provides 规则与
    // provide() 相同。
    //
    // registry 并不拥有这张表（见 published.hpp），也无法检查存储期 —
    // 类型系统中没有任何信息携带存储期。表若具有 static storage
    // duration，优先用下方的 publish_static()：它让编译器精确强制这条
    // 规则。本形式用于成员表。
    template <fixed_string Name, abi::Table T>
    const T& publish(const T& table) {
        constexpr std::uint64_t id = svc_id<Name>;
        if (!host_->provide_is_declared(index_, id))
            throw undeclared_provide(Name.view(), index_);
        host_->published().add(id, &table, Name.view());
        const std::size_t provider = index_;
        host_->record_provide(provider, id, std::string(Name.view()));
        scope_->defer([host = host_, provider, id] {
            host->published().remove(id);
            host->clear_provide(provider, id);
        });
        return table;
    }

    // publish() 的编译期受检形式，用于具有 **static storage duration** 的
    // 表。引用类型的非类型模板实参只能指名具有静态存储期的对象，因此发布
    // 局部对象 — 存储期不是类型的一部分，任何其他手段都抓不到这个错误 —
    // 会直接编译失败，而不是让 registry 留下一个指向死栈帧的指针
    // （docs/known-issues.md 的 P3-4）。
    //
    // 表若是组件的成员，请用 publish()：其生命周期属于组件，已由 scope
    // 管辖，而引用 NTTP 无法指名非静态成员。在这里传入局部对象（或局部
    // 对象的成员）会报 "no matching overload" — 不合法的模板实参没有
    // 位置可以挂一条更友好的错误信息。
    template <fixed_string Name, const auto& Table>
        requires abi::Table<std::remove_cvref_t<decltype(Table)>>
    void publish_static() {
        (void)publish<Name>(Table);
    }

    // 从同一 ABI 域解析已发布的表（宿主代码或另一组件）。带版本门槛：
    // 修订不兼容会以 plugin_error 报告，而不是报成 service 缺失 — 两种
    // 失败的修法完全不同。
    template <fixed_string Name, abi::Table T>
    const T* interface(std::uint32_t min_version = 1) const {
        constexpr std::uint64_t id = svc_id<Name>;
        const T* table = host_->published().template find_as<T>(id, min_version);
        if (table == nullptr) {
            if (host_->published().contains(id))
                throw plugin_error("nostos: published service '" + std::string(Name.view()) +
                                   "' exists but its interface revision is incompatible with " +
                                   "this consumer");
            throw missing_service(Name.view(), id);
        }
        host_->record_dependency(index_, id, std::string(Name.view()));
        return table;
    }

    // ---- events ----------------------------------------------------------

    template <EventType E, typename F>
        requires std::invocable<std::decay_t<F>&, const E&>
    [[nodiscard]] Subscription on(F&& fn) {
        return host_->events().on<E>(std::forward<F>(fn));
    }

    // 订阅事件并把 token 直接交给本组件的 scope，使订阅随组件做过的一切
    // 一起 teardown。多数组件应该用这个形式：单用 on() 返回的 token 必须
    // 妥善持有，token 被丢弃时会在语句末尾退订 — 这是静默空操作而非
    // 错误（见 tests/test_event.cpp 与 docs/known-issues.md 的 P2-7）。
    // 需要订阅活得比某个 scope 更久、或要提前取消时，才用 on()。
    template <EventType E, typename F>
        requires std::invocable<std::decay_t<F>&, const E&>
    void on_scoped(F&& fn) {
        scope_->own(host_->events().on<E>(std::forward<F>(fn)));
    }

    template <EventType E>
    void emit(const E& e) {
        host_->events().emit(e);
    }

    // ---- 调度接缝 ----------------------------------------------------

    // 本宿主运行所用的 executor。想延后工作的组件（「等当前激活/dispatch
    // 完成之后再做」）把工作投递（post）到它上面，而不是就地执行；投递
    // 之后何时运行由宿主决定（Host::set_executor，见 executor.hpp）。内核
    // 自身从不投递 — 一切都在调用者线程上同步执行。
    Executor& executor() const noexcept { return host_->executor(); }

    // ---- 逃生口（host 侧接线、测试） -------------------------

    ServiceRegistry& services() const noexcept { return host_->services(); }
    EventHub& events() const noexcept { return host_->events(); }
    HostCore& host() const noexcept { return *host_; }

private:
    HostCore* host_;
    std::size_t index_;
    Scope* scope_;
};

}  // namespace nostos
