// UPnP port mapping through the Windows NATUPnP COM API (IUPnPNAT), plus the local outbound address.
#include "upnp.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <natupnp.h>
#include <cstdio>

namespace h7::net {

std::string LocalIp() {
    static bool started = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    if (!started) return {};
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

static std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); } // ASCII only here
static std::string Narrow(BSTR b) {
    if (!b) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, b, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, b, -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct Bstr {
    BSTR b;
    explicit Bstr(const std::wstring& s) : b(SysAllocString(s.c_str())) {}
    ~Bstr() { SysFreeString(b); }
};

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};

static std::string HrText(const char* step, HRESULT hr) {
    char buf[160];
    snprintf(buf, sizeof buf, "%s failed (HRESULT 0x%08lX)", step, (unsigned long)hr);
    return buf;
}

// Runs fn(collection) with COM initialised on this thread.
template <class Fn> static UpnpResult WithMappings(Fn fn) {
    UpnpResult res;
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Com<IUPnPNAT> nat;
        HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), (void**)&nat);
        if (FAILED(hr)) {
            res.error = HrText("UPnPNAT", hr);
        } else {
            // The collection is null until the router has answered the discovery; retry for a few seconds.
            Com<IStaticPortMappingCollection> maps;
            for (int i = 0; i < 6 && !maps.p; ++i) {
                hr = nat->get_StaticPortMappingCollection(&maps);
                if (!maps.p) Sleep(1000);
            }
            if (!maps.p) res.error = "no UPnP router found (UPnP may be disabled on the router)";
            else fn(maps.p, res);
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return res;
}

UpnpResult UpnpAdd(int externalPort, int internalPort, const std::string& protocol, const std::string& description) {
    std::string local = LocalIp();
    UpnpResult r = WithMappings([&](IStaticPortMappingCollection* maps, UpnpResult& res) {
        res.internalIp = local;
        if (local.empty()) { res.error = "no network connection"; return; }
        Bstr proto(Widen(protocol)), client(Widen(local)), desc(Widen(description));
        // Replace a stale mapping of ours (or the user's) on the same port.
        maps->Remove(externalPort, proto.b);
        Com<IStaticPortMapping> m;
        HRESULT hr = maps->Add(externalPort, proto.b, internalPort, client.b, VARIANT_TRUE, desc.b, &m);
        if (FAILED(hr) || !m.p) { res.error = HrText("adding the port mapping", hr); return; }
        BSTR ext = nullptr;
        if (SUCCEEDED(m->get_ExternalIPAddress(&ext))) { res.externalIp = Narrow(ext); SysFreeString(ext); }
        res.ok = true;
    });
    if (r.internalIp.empty()) r.internalIp = local;
    return r;
}

UpnpResult UpnpRemove(int externalPort, const std::string& protocol) {
    return WithMappings([&](IStaticPortMappingCollection* maps, UpnpResult& res) {
        Bstr proto(Widen(protocol));
        HRESULT hr = maps->Remove(externalPort, proto.b);
        if (FAILED(hr)) res.error = HrText("removing the port mapping", hr);
        else res.ok = true;
    });
}

} // namespace h7::net
