// Hosting self-checks (selfcheck.h): this process's UDP sockets and Windows Firewall's verdict for inbound traffic.
#include "selfcheck.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netfw.h>
#include <cwchar>
#include <string>
#include <vector>

namespace h7::net {

std::vector<UdpEndpoint> ProcessUdpPorts() {
    std::vector<UdpEndpoint> out;
    DWORD size = 0;
    if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER) return out;
    std::vector<unsigned char> buf(size + 1024); // the table may grow between the two calls
    size = (DWORD)buf.size();
    if (GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) return out;
    auto* table = (MIB_UDPTABLE_OWNER_PID*)buf.data();
    DWORD me = GetCurrentProcessId();
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwOwningPid != me) continue;
        in_addr a;
        a.s_addr = row.dwLocalAddr;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a, ip, sizeof ip);
        out.push_back({ip, (int)ntohs((u_short)row.dwLocalPort)});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Windows Firewall (INetFwPolicy2)
// ---------------------------------------------------------------------------------------------------------------

static std::string Narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Expand(const wchar_t* path) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(path, buf, (DWORD)std::size(buf));
    return (n > 0 && n <= std::size(buf)) ? std::wstring(buf) : std::wstring(path);
}

// "*", "", "27015", "27000-27050,3074", keywords like "RPC" (never our port).
static bool PortListHas(const wchar_t* list, int port, bool emptyMeansAll) {
    if (!list || !*list) return emptyMeansAll;
    if (wcscmp(list, L"*") == 0) return true;
    const wchar_t* p = list;
    while (*p) {
        wchar_t* end = nullptr;
        long lo = wcstol(p, &end, 10);
        long hi = lo;
        if (end == p) { // a keyword: skip to the next item
            while (*p && *p != L',') ++p;
            if (*p) ++p;
            continue;
        }
        p = end;
        if (*p == L'-') hi = wcstol(p + 1, &end, 10), p = end;
        if (port >= lo && port <= hi) return true;
        while (*p && *p != L',') ++p;
        if (*p) ++p;
    }
    return false;
}

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

template <class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};

FirewallResult FirewallCheck(const std::string& protocol, int port) {
    FirewallResult r;
    ComInit com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) { r.error = "COM unavailable"; return r; }
    Ptr<INetFwPolicy2> pol;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), (void**)&pol))) {
        r.error = "Windows Firewall settings are not readable";
        return r;
    }
    wchar_t exeBuf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exeBuf, (DWORD)std::size(exeBuf));
    std::wstring exe(exeBuf, n);
    long ipProto = protocol == "TCP" ? NET_FW_IP_PROTOCOL_TCP : NET_FW_IP_PROTOCOL_UDP;

    long current = 0;
    pol->get_CurrentProfileTypes(&current);
    const struct { long bit; const char* name; } kProfiles[] = {
        {NET_FW_PROFILE2_DOMAIN, "domain"}, {NET_FW_PROFILE2_PRIVATE, "private"}, {NET_FW_PROFILE2_PUBLIC, "public"}};
    long enabledMask = 0;
    bool blockAll = false, defaultAllows = false;
    for (auto& pr : kProfiles) {
        if (!(current & pr.bit)) continue;
        if (!r.profiles.empty()) r.profiles += ", ";
        r.profiles += pr.name;
        VARIANT_BOOL on = VARIANT_FALSE;
        pol->get_FirewallEnabled((NET_FW_PROFILE_TYPE2)pr.bit, &on);
        if (!on) continue;
        enabledMask |= pr.bit;
        VARIANT_BOOL all = VARIANT_FALSE;
        pol->get_BlockAllInboundTraffic((NET_FW_PROFILE_TYPE2)pr.bit, &all);
        if (all) blockAll = true;
        NET_FW_ACTION def = NET_FW_ACTION_BLOCK;
        pol->get_DefaultInboundAction((NET_FW_PROFILE_TYPE2)pr.bit, &def);
        if (def == NET_FW_ACTION_ALLOW) defaultAllows = true;
    }
    r.ok = true;
    r.enabled = enabledMask != 0;
    if (!r.enabled) { r.verdict = "off"; return r; }
    if (blockAll) { r.verdict = "blocked"; r.rule = "Block all incoming connections"; return r; }

    Ptr<INetFwRules> rules;
    Ptr<IUnknown> unk;
    Ptr<IEnumVARIANT> it;
    if (FAILED(pol->get_Rules(&rules)) || FAILED(rules->get__NewEnum(&unk)) ||
        FAILED(unk->QueryInterface(__uuidof(IEnumVARIANT), (void**)&it))) {
        r.ok = false;
        r.error = "firewall rules are not readable";
        return r;
    }
    std::string allowRule, blockRule;
    VARIANT v;
    VariantInit(&v);
    while (it->Next(1, &v, nullptr) == S_OK) {
        Ptr<INetFwRule> rule;
        if (v.vt == VT_DISPATCH && v.pdispVal) v.pdispVal->QueryInterface(__uuidof(INetFwRule), (void**)&rule);
        VariantClear(&v);
        if (!rule.p) continue;
        VARIANT_BOOL on = VARIANT_FALSE;
        NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
        long profiles = 0, proto = 0;
        if (FAILED(rule->get_Enabled(&on)) || !on) continue;
        if (FAILED(rule->get_Direction(&dir)) || dir != NET_FW_RULE_DIR_IN) continue;
        if (FAILED(rule->get_Profiles(&profiles)) || !(profiles & enabledMask)) continue;
        if (FAILED(rule->get_Protocol(&proto)) || (proto != ipProto && proto != NET_FW_IP_PROTOCOL_ANY)) continue;
        BSTR app = nullptr, ports = nullptr, name = nullptr;
        rule->get_ApplicationName(&app);
        rule->get_LocalPorts(&ports);
        bool match;
        if (app && *app) match = _wcsicmp(Expand(app).c_str(), exe.c_str()) == 0 && PortListHas(ports, port, true);
        else match = ports && *ports && wcscmp(ports, L"*") != 0 && PortListHas(ports, port, false); // port rules for any program
        if (match) {
            NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
            rule->get_Action(&action);
            rule->get_Name(&name);
            std::string& slot = action == NET_FW_ACTION_BLOCK ? blockRule : allowRule;
            if (slot.empty()) slot = Narrow(name);
            if (slot.empty()) slot = "(unnamed rule)";
        }
        SysFreeString(app);
        SysFreeString(ports);
        SysFreeString(name);
    }
    if (!blockRule.empty()) { r.verdict = "blocked"; r.rule = blockRule; }
    else if (!allowRule.empty()) { r.verdict = "allowed"; r.rule = allowRule; }
    else r.verdict = defaultAllows ? "allowed" : "no rule";
    return r;
}

} // namespace h7::net
