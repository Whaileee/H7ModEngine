#pragma once
#include <string>

namespace h7::net {

struct UpnpResult {
    bool ok = false;
    bool existing = false;  // the port was already forwarded to this PC (e.g. by the game's own UPnP)
    std::string externalIp; // as reported by the router (may be empty)
    std::string internalIp; // the LAN address the mapping points to
    std::string error;
};

// Spoken directly to the router (SSDP + SOAP), not through Windows' UPnP component. Blocking (the router search
// can take several seconds): call from a worker thread.
// protocol: "UDP" or "TCP". Maps externalPort -> this PC's LAN address : internalPort.
UpnpResult UpnpAdd(int externalPort, int internalPort, const std::string& protocol, const std::string& description);
UpnpResult UpnpRemove(int externalPort, const std::string& protocol);

// The LAN address Windows would use to reach the internet ("" if offline). No packets are sent.
std::string LocalIp();

} // namespace h7::net
