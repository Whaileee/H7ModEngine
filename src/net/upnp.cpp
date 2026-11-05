// UPnP IGD port mapping, spoken directly to the router: SSDP discovery from this PC's LAN address, the router's
// device description over HTTP, then SOAP actions on its WANIPConnection / WANPPPConnection service.
//
// Not Windows' IUPnPNAT: on the user's PC it found no router while the router answered the game's own UPnP
// (which had mapped UDP 27015 itself). A virtual network adapter (VMware) is the likely cause; sending the
// discovery from the LAN address that reaches the internet avoids that (2026-11-05).
#include "upnp.h"
#include "http.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

namespace h7::net {

static bool StartWinsock() {
    static bool started = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return started;
}

std::string LocalIp() {
    if (!StartWinsock()) return {};
    // "Connecting" a UDP socket only picks a route; nothing is sent.
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    std::string out;
    if (connect(s, (sockaddr*)&to, sizeof to) == 0) {
        sockaddr_in me{};
        int len = sizeof me;
        if (getsockname(s, (sockaddr*)&me, &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf);
            out = buf;
        }
    }
    closesocket(s);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Small text helpers (router XML is simple enough for substring parsing)
// ---------------------------------------------------------------------------------------------------------------

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Text of the first <tag>...</tag> (any namespace prefix) at or after `from`; `at` gets the position after it.
static std::string Tag(const std::string& xml, const std::string& tag, size_t from = 0, size_t* at = nullptr) {
    std::string lx = Lower(xml), lt = Lower(tag);
    for (size_t p = lx.find('<', from); p != std::string::npos; p = lx.find('<', p + 1)) {
        size_t close = lx.find('>', p);
        if (close == std::string::npos) break;
        std::string name = lx.substr(p + 1, close - p - 1);
        size_t sp = name.find_first_of(" \t\r\n/");
        if (sp != std::string::npos) name = name.substr(0, sp);
        size_t colon = name.find(':');
        if (colon != std::string::npos) name = name.substr(colon + 1);
        if (name != lt) continue;
        size_t end = lx.find("</", close);
        while (end != std::string::npos) {
            size_t endClose = lx.find('>', end);
            std::string en = lx.substr(end + 2, endClose - end - 2);
            size_t c2 = en.find(':');
            if (c2 != std::string::npos) en = en.substr(c2 + 1);
            if (Trim(en) == lt) {
                if (at) *at = endClose + 1;
                return Trim(xml.substr(close + 1, end - close - 1));
            }
            end = lx.find("</", end + 2);
        }
        break;
    }
    return {};
}

// "http://host:port" of a URL.
static std::string Origin(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    size_t path = url.find('/', scheme + 3);
    return path == std::string::npos ? url : url.substr(0, path);
}

// ---------------------------------------------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------------------------------------------

struct Gateway {
    std::string controlUrl;
    std::string serviceType;
};

static std::mutex g_mx;      // one UPnP conversation at a time; routers handle overlapping ones badly
static Gateway g_gateway;    // cached after the first successful discovery

// M-SEARCH from `localIp`; returns the first LOCATION a gateway answers with.
static std::string Discover(const std::string& localIp, std::string& error) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { error = "cannot open a UDP socket"; return {}; }
    sockaddr_in me{};
    me.sin_family = AF_INET;
    inet_pton(AF_INET, localIp.c_str(), &me.sin_addr);
    if (bind(s, (sockaddr*)&me, sizeof me) != 0) { closesocket(s); error = "cannot bind to " + localIp; return {}; }
    in_addr iface = me.sin_addr;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&iface, sizeof iface);
    DWORD ttl = 2;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    const char* targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    std::string location;
    for (int attempt = 0; attempt < 3 && location.empty(); ++attempt) {
        for (const char* st : targets) {
            char msg[256];
            int n = snprintf(msg, sizeof msg,
                             "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                             "ST: %s\r\n\r\n", st);
            sendto(s, msg, n, 0, (sockaddr*)&to, sizeof to);
        }
        ULONGLONG until = GetTickCount64() + 2500;
        while (location.empty()) {
            ULONGLONG now = GetTickCount64();
            if (now >= until) break;
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(s, &fds);
            timeval tv{0, (long)std::min<ULONGLONG>(until - now, 500) * 1000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            char buf[2048];
            int got = recv(s, buf, sizeof buf - 1, 0);
            if (got <= 0) continue;
            buf[got] = 0;
            std::string reply(buf), lr = Lower(reply);
            size_t p = lr.find("\nlocation:");
            if (p == std::string::npos) continue;
            size_t end = reply.find("\r\n", p + 1);
            location = Trim(reply.substr(p + 10, end - p - 10));
        }
    }
    closesocket(s);
    if (location.empty()) error = "no UPnP router answered (UPnP may be off on the router)";
    return location;
}

// The router's WAN connection service from its device description.
static bool FindService(const std::string& location, Gateway& gw, std::string& error) {
    HttpRequest req;
    req.url = location;
    req.timeoutMs = 4000;
    HttpResponse res = HttpSend(req, "H7ModEngine");
    if (!res.ok || res.status != 200) {
        error = "router description not readable (" + (res.ok ? std::to_string(res.status) : res.error) + ")";
        return false;
    }
    std::string base = Tag(res.body, "URLBase");
    if (base.empty()) base = Origin(location);
    size_t pos = 0;
    for (;;) {
        size_t after = 0;
        std::string type = Tag(res.body, "serviceType", pos, &after);
        if (type.empty()) break;
        pos = after;
        if (type.find("WANIPConnection") == std::string::npos && type.find("WANPPPConnection") == std::string::npos) continue;
        std::string control = Tag(res.body, "controlURL", pos);
        if (control.empty()) continue;
        if (control.rfind("http", 0) != 0) {
            if (!base.empty() && base.back() == '/') base.pop_back();
            control = base + (control[0] == '/' ? "" : "/") + control;
        }
        gw.controlUrl = control;
        gw.serviceType = type;
        return true;
    }
    error = "the router offers no port mapping service";
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// SOAP
// ---------------------------------------------------------------------------------------------------------------

struct SoapResult {
    bool ok = false;
    std::string body;
    std::string error; // "UPnP error 718: ConflictInMappingEntry", transport errors, ...
};

static SoapResult Soap(const Gateway& gw, const std::string& action, const std::string& args) {
    HttpRequest req;
    req.method = "POST";
    req.url = gw.controlUrl;
    req.timeoutMs = 5000;
    req.headers = {{"Content-Type", "text/xml; charset=\"utf-8\""},
                   {"SOAPAction", "\"" + gw.serviceType + "#" + action + "\""}};
    req.body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action +
               " xmlns:u=\"" + gw.serviceType + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
    HttpResponse res = HttpSend(req, "H7ModEngine");
    SoapResult r;
    r.body = res.body;
    if (!res.ok) { r.error = res.error; return r; }
    if (res.status == 200) { r.ok = true; return r; }
    std::string code = Tag(res.body, "errorCode"), desc = Tag(res.body, "errorDescription");
    r.error = code.empty() ? ("HTTP " + std::to_string(res.status)) : ("UPnP error " + code + (desc.empty() ? "" : ": " + desc));
    return r;
}

static std::string Arg(const char* name, const std::string& value) {
    return std::string("<") + name + ">" + value + "</" + name + ">";
}

static std::string MappingKey(int port, const std::string& protocol) {
    return Arg("NewRemoteHost", "") + Arg("NewExternalPort", std::to_string(port)) + Arg("NewProtocol", protocol);
}

// Finds the gateway (cached), or explains why not.
static bool Gate(Gateway& gw, const std::string& localIp, std::string& error) {
    if (!g_gateway.controlUrl.empty()) { gw = g_gateway; return true; }
    std::string location = Discover(localIp, error);
    if (location.empty() || !FindService(location, gw, error)) return false;
    g_gateway = gw;
    return true;
}

UpnpResult UpnpAdd(int externalPort, int internalPort, const std::string& protocol, const std::string& description) {
    UpnpResult r;
    std::lock_guard lock(g_mx);
    if (!StartWinsock()) { r.error = "network unavailable"; return r; }
    r.internalIp = LocalIp();
    if (r.internalIp.empty()) { r.error = "no network connection"; return r; }
    Gateway gw;
    if (!Gate(gw, r.internalIp, r.error)) return r;

    // Already forwarded to this PC (e.g. by the game's own UPnP, which maps its port as "Heroes VII <mac>")? Then
    // the port is open and the mapping isn't ours: report it as existing so the caller doesn't remove it later.
    SoapResult get = Soap(gw, "GetSpecificPortMappingEntry", MappingKey(externalPort, protocol));
    std::string client = get.ok ? Tag(get.body, "NewInternalClient") : "";
    std::string port = get.ok ? Tag(get.body, "NewInternalPort") : "";
    if (client == r.internalIp && port == std::to_string(internalPort)) {
        r.ok = true;
        r.existing = true;
    } else {
        SoapResult add = Soap(gw, "AddPortMapping",
                              MappingKey(externalPort, protocol) + Arg("NewInternalPort", std::to_string(internalPort)) +
                              Arg("NewInternalClient", r.internalIp) + Arg("NewEnabled", "1") +
                              Arg("NewPortMappingDescription", description) + Arg("NewLeaseDuration", "0"));
        if (add.ok) r.ok = true;
        else if (!client.empty()) r.error = "port " + std::to_string(externalPort) + " is already forwarded to " + client;
        else r.error = "the router refused the mapping (" + add.error + ")";
    }
    SoapResult ip = Soap(gw, "GetExternalIPAddress", "");
    if (ip.ok) r.externalIp = Tag(ip.body, "NewExternalIPAddress");
    if (!r.ok && !ip.ok) g_gateway = {}; // the router stopped answering: rediscover next time
    return r;
}

UpnpResult UpnpRemove(int externalPort, const std::string& protocol) {
    UpnpResult r;
    std::lock_guard lock(g_mx);
    if (!StartWinsock()) { r.error = "network unavailable"; return r; }
    r.internalIp = LocalIp();
    Gateway gw;
    if (!Gate(gw, r.internalIp, r.error)) return r;
    SoapResult del = Soap(gw, "DeletePortMapping", MappingKey(externalPort, protocol));
    r.ok = del.ok;
    if (!del.ok) r.error = del.error;
    return r;
}

} // namespace h7::net
