#pragma once
// nostos-kit —— 极简 HTTP/1.1 服务器（Tier 1）。
//
// 为什么是"单线程 select 循环 + 每客户端有界缓冲"，而不是"每连接一线程"：
//   · 视频流最怕慢客户端。select 循环里，一个读得慢的连接只是自己缓冲填满、被丢弃，
//     **不可能**拖住别人（线程模型下要额外靠 SO_SNDTIMEO 兜）。
//   · 观众数 = 内存上限 × 常数，线程数恒为 1；deactivate 只需置位 + join 一次，
//     "关停后线程归零"因此是构造性成立的，而不是靠纪律。
//   · 代价：文件读是同步的，若根目录在网络盘上，一次 128 KiB 读会短暂卡住整个循环。
//     本地盘可接受。
//
// 本文件**不包含任何平台头**（实现里才有 <winsock2.h> / <sys/socket.h>），
// 这样 plugin.cpp 与测试都只看到纯 C++ 接口。

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nostos::kit::http {

// ---- 请求 -------------------------------------------------------------------

struct Header {
    std::string name;   // 已转小写
    std::string value;
};

struct Request {
    std::string method;   // "GET" / "HEAD" / "POST" ...
    std::string target;   // 原样保留（含 query）
    std::vector<Header> headers;
    std::string body;

    // 连接寻址（挂起-补响应）：handler 返回挂起响应后，真响应经
    // Server::respond(conn_id, resp) 补发。同步响应不读此值。
    std::uint64_t conn_id = 0;

    // 大小写无关地取一个头；没有返回空串。
    std::string header(std::string_view name) const;
};

// ---- 响应 -------------------------------------------------------------------

// 三种体：内存体（页面/JSON）、文件区间（流式）、空体（HEAD / 204）。
struct Response {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::vector<Header> headers;   // 追加头，例如 Accept-Ranges / Content-Range

    std::string body;              // 内存体
    std::string file_path;         // 非空 ⇒ 文件体（服务器按 offset/length 流式发送）
    std::uint64_t file_offset = 0;
    std::uint64_t file_length = 0;

    bool head_only = false;        // HEAD：只发头（Content-Length 仍按真实长度给）

    // SSE：回应 text/event-stream 头并**保持连接**；此后 Server::broadcast 会把
    // 每条消息以 "data: <text>\n\n" 推给所有 SSE 连接。body/file_* 被忽略。
    bool sse_stream = false;

    // 挂起-补响应：true ⇒ 连接保持、本轮不发送任何字节；真响应（含状态与头）
    // 稍后经 Server::respond(request.conn_id, resp) 从任意线程补发。与 sse_stream
    // 互斥（同为"连接保持"形态，不叠加）；挂起期间连接不受 stall_timeout 约束。
    // 重活从此离开 select 线程：handler 只留参数解析与投递（poster），消费方
    // 数据面锁与连接表锁的锁序因此单向化，跨线程 ABBA 无从构成。
    bool suspended = false;

    static Response text(int status, std::string content_type, std::string body);
    static Response status_page(int status, std::string_view note);
};

// 服务器把请求交给它。**在 select 线程上同步调用**，因此必须快（不许阻塞、不许 sleep）。
// 逃逸口：重活经 poster 投递到别的线程并返回 suspended 响应（见 Response::suspended），
// select 线程上只留参数解析与投递；真响应稍后经 Server::respond 补发。
class Handler {
public:
    virtual ~Handler() = default;
    virtual Response handle(const Request& request) = 0;

    // 每轮循环调用一次（可用于把统计搬到别处、清理过期状态）。
    virtual void on_tick() {}
};

// ---- 服务器 -----------------------------------------------------------------

struct ServerOptions {
    std::size_t max_clients = 16;         // 同时在线的连接数（含正在传的流）
    std::size_t max_streams = 8;          // 同时进行的文件流；超出 ⇒ 503
    std::size_t buffer_cap = 256 * 1024;  // 每客户端"待发"上限（背压闸门，内存有界）
    std::size_t chunk = 128 * 1024;       // 文件分块大小
    // 单请求累计缓冲上限：头+body 合并计入，recv 每轮追加后即查，超出 ⇒ 413。
    // 默认给足对话类应用"输入即粘贴"的主通道，仍远小于会话上下文，内存有界
    // （每客户端 buffer_cap 背压照旧兜底）。
    // 踩过: 16KB 默认下粘贴 35KB 材料即被拒。
    std::size_t max_request = 2 * 1024 * 1024;
    std::chrono::milliseconds stall_timeout{15000};       // 完全无进展 ⇒ 丢弃
                                                          // （SSE 长连接豁免：空闲是常态）
    std::chrono::milliseconds poll_interval{200};         // select 超时（也决定 stop 的响应延迟）
};

struct ServerStats {
    std::uint64_t requests = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t stalled_dropped = 0;
    std::uint64_t rejected = 0;      // 超出 max_clients / max_streams
    std::uint64_t bad_requests = 0;  // 解析失败 / 请求过大
    // handler 在 select 线程持 clients_mu_ 同步执行，其回调链里的 broadcast 属同
    // 线程非递归锁二次进入，闸门直接丢弃并计数——正常恒 0，非 0 即 handler 回调
    // 链里有广播（跨线程锁序问题闸门不管，由消费方锁序纪律维持）。
    std::uint64_t reentrant_broadcasts = 0;
    std::size_t active_clients = 0;
    std::size_t active_streams = 0;
};

// 监听 127.0.0.1:<port>（port == 0 时由系统分配，用 port() 读回）。
// 失败时 error 写入原因，ok() 为 false。
class Server {
public:
    Server(std::uint16_t port, Handler& handler, ServerOptions options, std::string* error);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool ok() const noexcept;
    std::uint16_t port() const noexcept;

    // 幂等。置停止位并唤醒循环（延迟 ≤ poll_interval）；析构时 join。
    void stop() noexcept;

    // 向所有 SSE 连接广播一行（线程安全；数据以 "data: <text>\n\n" 帧发送）。
    // 慢客户端（待发缓冲达到 buffer_cap）跳过本条并计入 stalled_dropped——
    // 单个读不动的浏览器不能拖住别人，这与整个服务器的背压设计一致。
    void broadcast(std::string_view text);

    // 补发一个挂起响应（任意线程）：按 conn_id 寻址连接并发送完整响应。连接已
    // 断开/不存在/未处于挂起态 ⇒ 静默丢弃（客户端关页是常态路径，非错误）。
    // 这是 handler 返回 suspended 的另一半契约——挂起连接唯一能出响应的出口。
    // 补发路径只拿连接表锁：挂起模型下补发方此前已做完消费方数据面操作、不持
    // 任何插件侧锁，与 select 线程的快进快出 handler 构成单向锁序。
    void respond(std::uint64_t conn_id, const Response& response);

    ServerStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nostos::kit::http
