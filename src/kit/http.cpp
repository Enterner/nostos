#include <nostos/kit/http.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace nostos::kit::http {
namespace {

void close_socket(socket_t fd) noexcept {
    if (fd == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

bool would_block() noexcept {
#if defined(_WIN32)
    return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

#if defined(_WIN32)
struct SocketRuntime {
    SocketRuntime() {
        WSADATA data{};
        ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketRuntime() {
        if (ok_) ::WSACleanup();
    }
    bool ok() const noexcept { return ok_; }
    bool ok_ = false;
};
#else
struct SocketRuntime {
    bool ok() const noexcept { return true; }
};
#endif

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---- 请求解析 ---------------------------------------------------------------

// 只做"够用"的解析：请求行 + 头 + 可选 Content-Length 体。任何畸形都返回 false
// （调用点回 400 并关连接）—— 我们不是通用 web 服务器，宁可少支持也不要含糊。
bool parse_request(std::string_view raw, Request* out) {
    const std::size_t line_end = raw.find("\r\n");
    if (line_end == std::string_view::npos) return false;
    const std::string_view request_line = raw.substr(0, line_end);
    const std::size_t sp1 = request_line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    const std::size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;
    out->method = std::string(request_line.substr(0, sp1));
    out->target = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
    if (out->method.empty() || out->target.empty()) return false;

    std::size_t pos = line_end + 2;
    std::size_t content_length = 0;
    while (pos < raw.size()) {
        const std::size_t end = raw.find("\r\n", pos);
        if (end == std::string_view::npos) return false;
        if (end == pos) {  // 空行：头结束
            pos += 2;
            break;
        }
        const std::string_view line = raw.substr(pos, end - pos);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) return false;
        Header header;
        header.name = lower(line.substr(0, colon));
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        header.value = std::string(value);
        if (header.name == "content-length") {
            try {
                content_length = static_cast<std::size_t>(std::stoull(header.value));
            } catch (...) {
                return false;
            }
        }
        out->headers.push_back(std::move(header));
        pos = end + 2;
    }
    if (content_length > 0) {
        if (raw.size() < pos + content_length) return false;  // 体还没到齐
        out->body = std::string(raw.substr(pos, content_length));
    }
    return true;
}

std::string response_head(const Response& response, std::size_t content_length) {
    const char* reason = "OK";
    switch (response.status) {
        case 200: reason = "OK"; break;
        case 204: reason = "No Content"; break;
        case 206: reason = "Partial Content"; break;
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 413: reason = "Payload Too Large"; break;
        case 416: reason = "Range Not Satisfiable"; break;
        case 500: reason = "Internal Server Error"; break;
        case 503: reason = "Service Unavailable"; break;
        default: reason = "Error"; break;
    }
    std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " + reason + "\r\n";
    head += "Content-Type: " + response.content_type + "\r\n";
    head += "Content-Length: " + std::to_string(content_length) + "\r\n";
    // 面板/流式都不要缓存：每个响应都带这一头。
    head += "Cache-Control: no-store\r\n";
    for (const auto& header : response.headers)
        head += header.name + ": " + header.value + "\r\n";
    // 每条响应一个连接：省掉 keep-alive 状态机，浏览器取媒体本来也是"一段一连接"。
    head += "Connection: close\r\n\r\n";
    return head;
}

}  // namespace

std::string Request::header(std::string_view name) const {
    const std::string wanted = lower(name);
    for (const auto& header : headers)
        if (header.name == wanted) return header.value;
    return {};
}

Response Response::text(int status, std::string content_type, std::string body) {
    Response response;
    response.status = status;
    response.content_type = std::move(content_type);
    response.body = std::move(body);
    return response;
}

Response Response::status_page(int status, std::string_view note) {
    std::string body = "<!doctype html><meta charset=utf-8><title>nostos-kit</title>"
                       "<body style=\"font-family:system-ui;padding:2rem\"><h1>" +
                       std::to_string(status) + "</h1><p>" + std::string(note) + "</p></body>";
    return text(status, "text/html; charset=utf-8", std::move(body));
}

// ---- 实现 -------------------------------------------------------------------

struct Server::Impl {
    struct Client {
        socket_t fd = kInvalidSocket;
        std::string in;
        std::string out;                     // 待发字节（头 + 体 + 文件块），上限 buffer_cap
        std::unique_ptr<std::ifstream> file; // 每个客户端自己的句柄 ⇒ 无共享 seek 状态
        std::uint64_t remaining = 0;         // 文件体还剩多少字节
        bool headers_sent_queued = false;    // out 里已经含响应头
        bool is_stream = false;              // 占着一个 stream 槽位
        bool sse = false;                    // SSE 长连接（broadcast 的接收端）
        bool request_done = false;           // 请求已解析并响应
        std::chrono::steady_clock::time_point last_progress;
    };

    Impl(std::uint16_t wanted_port, Handler& handler, ServerOptions options, std::string* error)
        : handler_(&handler), options_(options) {
        if (!runtime_.ok()) {
            if (error != nullptr) *error = "Winsock 初始化失败";
            return;
        }
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ == kInvalidSocket) {
            if (error != nullptr) *error = "socket() 失败";
            return;
        }
        int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);  // 只监听本机
        address.sin_port = ::htons(wanted_port);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
            if (error != nullptr) *error = "bind(127.0.0.1:" + std::to_string(wanted_port) + ") 失败";
            close_socket(listener_);
            listener_ = kInvalidSocket;
            return;
        }
        if (::listen(listener_, 64) != 0) {
            if (error != nullptr) *error = "listen() 失败";
            close_socket(listener_);
            listener_ = kInvalidSocket;
            return;
        }
        // 实际端口（port == 0 时由系统分配）
        sockaddr_in bound{};
#if defined(_WIN32)
        int bound_len = sizeof bound;
#else
        socklen_t bound_len = sizeof bound;
#endif
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0)
            port_ = ::ntohs(bound.sin_port);

        set_non_blocking(listener_);
        thread_ = std::thread([this] { run(); });
    }

    ~Impl() {
        stop();
        if (thread_.joinable()) thread_.join();
        // 循环线程退出后，客户端已在 stop() 里关掉；这里只收监听 socket。
        close_socket(listener_);
        listener_ = kInvalidSocket;
    }

    static void set_non_blocking(socket_t fd) noexcept {
#if defined(_WIN32)
        u_long mode = 1;
        ::ioctlsocket(fd, FIONBIO, &mode);
#else
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> lock(clients_mu_);
        stopping_.store(true);
        // 关掉监听与所有客户端连接：既能立刻唤醒 select，也让卡在 read/send 的连接断掉。
        close_socket(listener_);
        listener_ = kInvalidSocket;
        for (auto& client : clients_) close_socket(client.fd);
        for (auto& client : clients_) client.fd = kInvalidSocket;
    }

    void run() {
        while (!stopping_.load()) {
            fd_set readable;
            fd_set writable;
            bool listening = false;
            socket_t max_fd = kInvalidSocket;
            {
                std::lock_guard<std::mutex> lock(clients_mu_);
                FD_ZERO(&readable);
                FD_ZERO(&writable);
                max_fd = listener_;
                listening = listener_ != kInvalidSocket;
                if (listening) FD_SET(listener_, &readable);
                for (const auto& client : clients_) {
                    FD_SET(client.fd, &readable);
                    if (!client.out.empty()) FD_SET(client.fd, &writable);
                    if (client.fd > max_fd) max_fd = client.fd;
                }
            }

            timeval timeout{};
            timeout.tv_sec = static_cast<long>(options_.poll_interval.count() / 1000);
            timeout.tv_usec = static_cast<long>((options_.poll_interval.count() % 1000) * 1000);
            const int ready = ::select(static_cast<int>(max_fd) + 1, &readable, &writable, nullptr, &timeout);
            if (ready < 0) {
#if defined(_WIN32)
                if (::WSAGetLastError() == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
#endif
                break;
            }

            if (ready > 0) {
                std::lock_guard<std::mutex> lock(clients_mu_);
                if (listening && listener_ != kInvalidSocket && FD_ISSET(listener_, &readable)) accept_ready();
                for (std::size_t i = 0; i < clients_.size(); ++i) {
                    Client& client = clients_[i];
                    if (client.fd == kInvalidSocket) continue;
                    if (FD_ISSET(client.fd, &writable)) flush(client);
                    if (client.fd != kInvalidSocket && FD_ISSET(client.fd, &readable)) read_ready(client);
                    if (client.fd != kInvalidSocket) fill_file(client);
                }
                reap();
            }
            handler_->on_tick();
        }
        std::lock_guard<std::mutex> lock(clients_mu_);
        for (auto& client : clients_) close_socket(client.fd);
        clients_.clear();
        active_streams_.store(0);
    }

    void accept_ready() {
        for (;;) {
            socket_t fd = ::accept(listener_, nullptr, nullptr);
            if (fd == kInvalidSocket) {
                if (would_block()) return;
                return;
            }
            if (clients_.size() >= options_.max_clients) {
                rejected_.fetch_add(1);
                close_socket(fd);  // 太多连接：直接关，不排队
                continue;
            }
            set_non_blocking(fd);
            Client client;
            client.fd = fd;
            client.last_progress = std::chrono::steady_clock::now();
            clients_.push_back(std::move(client));
        }
    }

    // 填文件数据；out 到达 buffer_cap 就停下 —— 这就是背压闸门（内存有界）。
    void fill_file(Client& client) {
        if (client.file == nullptr || client.remaining == 0) return;
        while (client.remaining > 0 && client.out.size() < options_.buffer_cap) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(options_.chunk, client.remaining));
            std::string block(want, '\0');
            client.file->read(block.data(), static_cast<std::streamsize>(want));
            const std::streamsize got = client.file->gcount();
            if (got <= 0) {  // 文件被截断/读失败：收尾，停止发送
                client.remaining = 0;
                client.file.reset();
                return;
            }
            block.resize(static_cast<std::size_t>(got));
            client.remaining -= static_cast<std::uint64_t>(got);
            client.out += block;
        }
    }

    void read_ready(Client& client) {
        char buffer[8192];
        for (;;) {
            const int got = ::recv(client.fd, buffer, sizeof buffer, 0);
            if (got > 0) {
                client.in.append(buffer, static_cast<std::size_t>(got));
                client.last_progress = std::chrono::steady_clock::now();
                if (client.in.size() > options_.max_request) {
                    respond(client, Response::status_page(413, "请求过大"));
                    return;
                }
                if (client.request_done) continue;  // 已经回应过，剩下的字节丢掉
                continue;
            }
            if (got == 0) {  // 对端关闭
                if (!client.request_done) close_client(client);
                return;
            }
            if (would_block()) break;
            close_client(client);
            return;
        }
        if (client.request_done) return;
        // 请求到齐了吗（头结束；带体的还要等体到齐）
        const std::size_t head_end = client.in.find("\r\n\r\n");
        if (head_end == std::string::npos) return;
        Request request;
        if (!parse_request(client.in, &request)) {
            // 可能是体还没到齐：只有当 Content-Length 已声明且不够时才等待，否则 400
            const std::string length_text = [&] {
                for (const auto& header : request.headers)
                    if (header.name == "content-length") return header.value;
                return std::string{};
            }();
            if (!length_text.empty()) return;  // 半包：等下一轮
            bad_requests_.fetch_add(1);
            respond(client, Response::status_page(400, "请求无法解析"));
            return;
        }
        client.request_done = true;
        requests_.fetch_add(1);
        dispatch(client, request);
    }

    void dispatch(Client& client, const Request& request) {
        Response response;
        try {
            response = handler_->handle(request);
        } catch (...) {
            response = Response::status_page(500, "处理请求时抛出异常");
        }

        // 并发流上限在这里统一兜住：handler 不需要（也不该）自己数。
        if (!response.file_path.empty() && !response.head_only) {
            if (active_streams_.load() >= options_.max_streams) {
                rejected_.fetch_add(1);
                Response busy = Response::status_page(503, "并发流已达上限，稍后重试");
                busy.headers.push_back({"Retry-After", "1"});
                respond(client, busy);
                return;
            }
        }
        respond(client, response);
    }

    void respond(Client& client, const Response& response) {
        if (response.sse_stream) {
            // 事件流：头发出后连接保持；此后只有 broadcast 会往 out 里写。
            client.out +=
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
            client.sse = true;
            client.last_progress = std::chrono::steady_clock::now();
            flush(client);
            return;
        }
        const std::size_t content_length =
            !response.file_path.empty() ? static_cast<std::size_t>(response.file_length)
                                        : response.body.size();
        client.out += response_head(response, content_length);
        if (!response.head_only) {
            if (!response.file_path.empty()) {
                auto file = std::make_unique<std::ifstream>(response.file_path, std::ios::binary);
                if (!file->good()) {
                    client.out.clear();
                    respond(client, Response::status_page(500, "打不开文件"));
                    return;
                }
                file->seekg(static_cast<std::streamoff>(response.file_offset), std::ios::beg);
                client.file = std::move(file);
                client.remaining = response.file_length;
                client.is_stream = true;
                active_streams_.fetch_add(1);
                fill_file(client);
            } else {
                client.out += response.body;
            }
        }
        client.last_progress = std::chrono::steady_clock::now();
        flush(client);
        maybe_finish(client);
    }

    void flush(Client& client) {
        while (!client.out.empty()) {
            const std::size_t want = client.out.size() > 64 * 1024 ? 64 * 1024 : client.out.size();
            const int sent = ::send(client.fd, client.out.data(), static_cast<int>(want), 0);
            if (sent > 0) {
                client.out.erase(0, static_cast<std::size_t>(sent));
                bytes_sent_.fetch_add(static_cast<std::uint64_t>(sent));
                client.last_progress = std::chrono::steady_clock::now();
                continue;
            }
            if (sent < 0 && would_block()) return;      // socket 满：留给下一轮 select
            close_client(client);                       // 对端断开（浏览器 seek 时是常态）
            return;
        }
        maybe_finish(client);
    }

    // 没有待发数据、也没有剩余文件 ⇒ 这条连接的事干完了，主动关（Connection: close）。
    void maybe_finish(Client& client) {
        if (client.sse) return;  // SSE 连接保持到 stop
        if (client.fd == kInvalidSocket) return;
        if (!client.out.empty()) return;
        if (client.file != nullptr && client.remaining > 0) return;
        if (!client.request_done) return;  // 还没收到请求，等它
        close_client(client);
    }

    void close_client(Client& client) {
        if (client.fd == kInvalidSocket) return;
        close_socket(client.fd);
        client.fd = kInvalidSocket;
        if (client.is_stream) {
            active_streams_.fetch_sub(1);
            client.is_stream = false;
        }
    }

    // 清掉已关闭的连接 + 丢弃"完全没进展"的连接（对端拔网线/假死时不让它占着槽位）。
    void reap() {
        const auto now = std::chrono::steady_clock::now();
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                      [&](Client& client) { return client.fd == kInvalidSocket; }),
                       clients_.end());
        for (auto& client : clients_) {
            if (client.sse) continue;  // SSE 长连接空闲是常态：两次事件之间本就无数据，
                                       // 不受停滞回收（踩过: 插件侧 webui——SSE 空闲 15s 被掐断，页面实时通道周期性死亡）。
            if (now - client.last_progress <= options_.stall_timeout) continue;
            stalled_dropped_.fetch_add(1);
            close_client(client);
        }
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                      [&](Client& client) { return client.fd == kInvalidSocket; }),
                       clients_.end());
        active_clients_.store(clients_.size());
    }

    // UI/宿主线程调用：给所有 SSE 连接追加一帧。慢客户端跳过（不阻塞广播方）。
    void broadcast(std::string_view text) {
        std::string frame = "data: ";
        frame.append(text);
        frame += "\n\n";
        std::lock_guard<std::mutex> lock(clients_mu_);
        for (auto& client : clients_) {
            if (!client.sse || client.fd == kInvalidSocket) continue;
            if (client.out.size() >= options_.buffer_cap) {
                stalled_dropped_.fetch_add(1);  // 背压闸门：这个浏览器跟不上了
                continue;
            }
            client.out += frame;
        }
    }

    Handler* handler_;
    ServerOptions options_;
    SocketRuntime runtime_;
    std::mutex clients_mu_;  // 守护 clients_ 与各 client 的 out/fd（循环线程 vs broadcast/stop）
    socket_t listener_ = kInvalidSocket;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::vector<Client> clients_;

    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> stalled_dropped_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> bad_requests_{0};
    std::atomic<std::size_t> active_streams_{0};
    std::atomic<std::size_t> active_clients_{0};
};

Server::Server(std::uint16_t port, Handler& handler, ServerOptions options, std::string* error)
    : impl_(std::make_unique<Impl>(port, handler, options, error)) {}

Server::~Server() = default;

bool Server::ok() const noexcept { return impl_ && impl_->listener_ != kInvalidSocket; }
std::uint16_t Server::port() const noexcept { return impl_ ? impl_->port_ : 0; }

void Server::stop() noexcept {
    if (impl_) impl_->stop();
}

void Server::broadcast(std::string_view text) {
    if (impl_) impl_->broadcast(text);
}

ServerStats Server::stats() const noexcept {
    ServerStats stats;
    if (!impl_) return stats;
    stats.requests = impl_->requests_.load();
    stats.bytes_sent = impl_->bytes_sent_.load();
    stats.stalled_dropped = impl_->stalled_dropped_.load();
    stats.rejected = impl_->rejected_.load();
    stats.bad_requests = impl_->bad_requests_.load();
    stats.active_clients = impl_->active_clients_.load();
    stats.active_streams = impl_->active_streams_.load();
    return stats;
}

}  // namespace nostos::kit::http
