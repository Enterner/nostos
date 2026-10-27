#pragma once
// nostos-kit —— Windows 平台件：一发 HTTP 请求（探测/拉清单形态）。
//
// 调用方需链 winhttp.lib（不调用则不引入任何导入）。仅 Windows 生效；其它平台
// 一律 sent=false。

#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace nostos::kit::win {

#if defined(_WIN32)

// 一次 HTTP 往返的结果。sent=false ⇒ 请求未抵达对端，gle 是失败阶段的
// GetLastError；sent=true 后看 status（HTTP 层语义），body 尽力而为整读。
struct HttpReply {
    bool sent = false;
    unsigned long status = 0;
    unsigned long gle = 0;
    std::string body;
};

struct HttpFetchOptions {
    const wchar_t* method = L"GET";
    const wchar_t* user_agent = L"nostos-kit/1.0";
    // 请求头逐项 "Name: Value"（宽字符，WinHttpAddRequestHeaders 原样收）。
    std::vector<std::wstring> headers;
    // 四段超时 ms：解析/连接/发送/接收（WinHttpSetTimeouts 语义）。
    int timeouts[4] = {3000, 3000, 3000, 3000};
    std::size_t body_cap = 256 * 1024;  // 响应体读取上限，到顶截断
};

// 一发 HTTP 请求：crack→open→connect→request→send→receive→status→body 收敛为
// 单调用。定位是"探测/拉清单"的短平快形态，不是通用客户端——固定 NO_PROXY
// 直连、不跟随重定向、body 一次性整读（body_cap 封顶）；流式/SSE 场景不适用。
inline HttpReply http_fetch(const std::wstring& url,
                            const HttpFetchOptions& options = {}) {
    HttpReply reply;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = 1;
    uc.dwHostNameLength = 1;
    uc.dwUrlPathLength = 1;
    uc.dwExtraInfoLength = 1;  // lpsz* 指 url 缓冲内部，crack 后立即取走拼合
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        reply.gle = GetLastError();
        return reply;
    }
    const std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                              std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    struct Handle {  // HINTERNET RAII：任何早退路径统一收口，析构序 req→con→ses
        HINTERNET h = nullptr;
        ~Handle() {
            if (h) WinHttpCloseHandle(h);
        }
        Handle() = default;  // 声明了拷贝删除即无隐式默认构造，须显式补
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    } ses, con, req;
    ses.h = WinHttpOpen(options.user_agent, WINHTTP_ACCESS_TYPE_NO_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses.h != nullptr)
        con.h = WinHttpConnect(ses.h,
                               std::wstring(uc.lpszHostName, uc.dwHostNameLength).c_str(),
                               uc.nPort, 0);
    if (con.h != nullptr)
        req.h = WinHttpOpenRequest(
            con.h, options.method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (req.h == nullptr) {
        reply.gle = GetLastError();
        return reply;
    }
    WinHttpSetTimeouts(req.h, options.timeouts[0], options.timeouts[1],
                       options.timeouts[2], options.timeouts[3]);
    for (const auto& header : options.headers)
        WinHttpAddRequestHeaders(req.h, header.c_str(), static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        reply.gle = GetLastError();
        return reply;
    }
    DWORD size = sizeof(reply.status);
    WinHttpQueryHeaders(req.h,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &reply.status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    reply.sent = true;
    for (;;) {  // 整读 body；读失败/对端提前收尾按已读部分交付
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req.h, chunk.data(), avail, &got) || got == 0) break;
        reply.body.append(chunk, 0, got);
        if (reply.body.size() >= options.body_cap) break;
    }
    return reply;
}

#endif  // _WIN32

}  // namespace nostos::kit::win
