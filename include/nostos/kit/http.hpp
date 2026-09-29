#pragma once
// nostos-kit —— 极简 HTTP/1.1 服务器（Tier 1；提取自 video_stream 的 http_local）。
//
// 为什么是"单线程 select 循环 + 每客户端有界缓冲"，而不是"每连接一线程"：
//   · 视频流最怕慢客户端。select 循环里，一个读得慢的连接只是自己缓冲填满、被丢弃，
//     **不可能**拖住别人（线程模型下要额外靠 SO_SNDTIMEO 兜）。
//   · 观众数 = 内存上限 × 常数，线程数恒为 1；deactivate 只需置位 + join 一次，
//     "关停后线程归零"因此是构造性成立的，而不是靠纪律。
//   · 代价：文件读是同步的，若根目录在网络盘上，一次 128 KiB 读会短暂卡住整个循环。
//     本地盘可接受，这条已记进方案的风险清单。
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

    static Response text(int status, std::string content_type, std::string body);
    static Response status_page(int status, std::string_view note);
};

// 服务器把请求交给它。**在 select 线程上同步调用**，因此必须快（不许阻塞、不许 sleep）。
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
    std::size_t max_request = 16 * 1024;  // 请求头上限
    std::chrono::milliseconds stall_timeout{15000};       // 完全无进展 ⇒ 丢弃
    std::chrono::milliseconds poll_interval{200};         // select 超时（也决定 stop 的响应延迟）
};

struct ServerStats {
    std::uint64_t requests = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t stalled_dropped = 0;
    std::uint64_t rejected = 0;      // 超出 max_clients / max_streams
    std::uint64_t bad_requests = 0;  // 解析失败 / 请求过大
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

    ServerStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nostos::kit::http
