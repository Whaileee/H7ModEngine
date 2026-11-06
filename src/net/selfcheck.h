#pragma once
#include <string>
#include <vector>

namespace h7::net {

// Hosting self-checks: which UDP ports this process really listens on, and whether Windows Firewall lets
// inbound traffic reach it. (The lobby can only test TCP; these explain a host the lobby verified but nobody
// can join, seen 2026-11-06.)

struct UdpEndpoint {
    std::string ip; // "0.0.0.0" for all addresses
    int port = 0;
};

// IPv4 UDP sockets owned by this process. Fast; any thread.
std::vector<UdpEndpoint> ProcessUdpPorts();

struct FirewallResult {
    bool ok = false;      // the check ran (false: error says why; third-party firewalls are not seen either way)
    bool enabled = false; // Windows Firewall is on for at least one active network profile
    std::string verdict;  // "allowed", "blocked", "no rule" (blocked by default; Windows only asks once), "off"
    std::string rule;     // the deciding rule's name, when there is one
    std::string profiles; // active profiles, e.g. "private" or "domain, public"
    std::string error;
};

// Would inbound `protocol` ("UDP"/"TCP") traffic to `port` reach this program (GetModuleFileName)? Evaluates
// the enabled inbound rules of the active profiles the way Windows Firewall does: a matching block rule wins over
// an allow rule; without either the profile's default inbound action decides. Blocking (COM): call from a worker.
FirewallResult FirewallCheck(const std::string& protocol, int port);

} // namespace h7::net
