// HTTP(S) client on WinHTTP (ships with Windows, uses the system's TLS and proxy settings).
#include "http.h"

#include <windows.h>
#include <winhttp.h>
#include <mutex>
#include <algorithm>

namespace h7::net {

static constexpr size_t kMaxBody = 16 * 1024 * 1024;

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string Narrow(const wchar_t* w, size_t len) {
    if (!len) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)len, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string ErrorText(const char* step, DWORD code) {
    char buf[256] = {};
    const char* what = nullptr;
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: what = "timed out"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: what = "host name not resolved"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: what = "cannot connect"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: what = "connection reset"; break;
    case ERROR_WINHTTP_SECURE_FAILURE: what = "TLS/certificate failure"; break;
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: what = "unsupported URL scheme"; break;
    case ERROR_WINHTTP_INVALID_URL: what = "invalid URL"; break;
    default: break;
    }
    if (what) snprintf(buf, sizeof buf, "%s: %s (%lu)", step, what, code);
    else snprintf(buf, sizeof buf, "%s failed (error %lu)", step, code);
    return buf;
}

// One session for the process; WinHTTP sessions are thread-safe.
static HINTERNET Session(const std::string& userAgent) {
    static std::once_flag once;
    static HINTERNET session = nullptr;
    std::call_once(once, [&] {
        session = WinHttpOpen(Widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1+
            session = WinHttpOpen(Widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    });
    return session;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

static void ParseHeaders(const std::wstring& raw, HttpResponse& res) {
    // "HTTP/1.1 200 OK\r\nName: value\r\n...\r\n\r\n"
    size_t pos = raw.find(L"\r\n");
    while (pos != std::wstring::npos) {
        size_t start = pos + 2;
        size_t end = raw.find(L"\r\n", start);
        if (end == std::wstring::npos || end == start) break;
        size_t colon = raw.find(L':', start);
        if (colon != std::wstring::npos && colon < end) {
            std::string name = Narrow(raw.data() + start, colon - start);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)tolower(c); });
            size_t v = colon + 1;
            while (v < end && raw[v] == L' ') ++v;
            res.headers.emplace_back(name, Narrow(raw.data() + v, end - v));
        }
        pos = end;
    }
}

HttpResponse HttpSend(const HttpRequest& req, const std::string& userAgent) {
    HttpResponse res;
    HINTERNET session = Session(userAgent);
    if (!session) { res.error = ErrorText("WinHttpOpen", GetLastError()); return res; }

    std::wstring url = Widen(req.url);
    URL_COMPONENTS uc{sizeof uc};
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { res.error = ErrorText("parse URL", GetLastError()); return res; }
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!https && uc.nScheme != INTERNET_SCHEME_HTTP) { res.error = "only http:// and https:// URLs are supported"; return res; }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    Handle conn{WinHttpConnect(session, host.c_str(), uc.nPort, 0)};
    if (!conn.h) { res.error = ErrorText("connect", GetLastError()); return res; }
    Handle request{WinHttpOpenRequest(conn.h, Widen(req.method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.h) { res.error = ErrorText("open request", GetLastError()); return res; }
    int t = std::max(req.timeoutMs, 1);
    WinHttpSetTimeouts(request.h, t, t, t, t);

    std::wstring headers;
    for (auto& [k, v] : req.headers) headers += Widen(k) + L": " + Widen(v) + L"\r\n";
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : (DWORD)-1L, req.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)req.body.data(),
                            (DWORD)req.body.size(), (DWORD)req.body.size(), 0)) {
        res.error = ErrorText("send", GetLastError());
        return res;
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) { res.error = ErrorText("receive", GetLastError()); return res; }

    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    res.status = (int)status;

    size = 0;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && size) {
        std::wstring raw(size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                &size, WINHTTP_NO_HEADER_INDEX))
            ParseHeaders(raw, res);
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) { res.error = ErrorText("read", GetLastError()); return res; }
        if (!avail) break;
        if (res.body.size() + avail > kMaxBody) { res.error = "response body larger than 16 MB"; return res; }
        size_t at = res.body.size();
        res.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(request.h, res.body.data() + at, avail, &got)) {
            res.error = ErrorText("read", GetLastError());
            return res;
        }
        res.body.resize(at + got);
    }
    res.ok = true;
    return res;
}

} // namespace h7::net
