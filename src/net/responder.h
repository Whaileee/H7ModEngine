#pragma once
#include <string>

namespace h7::net {

// The host side of the lobby's TCP challenge (docs/ONLINE.md "Host verification"). A listener thread on TCP
// `port` answers, one connection at a time, exactly one line:
//   in   "H7LOBBY 1 " + 32 lowercase hex (nonce) + "\n"       43 bytes
//   out  hex(HMAC-SHA256(key = key, msg = nonce)) + "\n"      65 bytes
// and closes silently on anything else or after 2 s. Runs on its own thread because the game thread pauses while
// the window is unfocused.
//
// Returns "" when listening, otherwise the reason. Calling it while running on the same port only swaps the key;
// on another port the old listener is stopped first.
std::string ResponderStart(int port, const std::string& key);
void ResponderStop(); // closes the listener and waits for the thread (at most ~2 s while a connection is open)
bool ResponderRunning();

// Tests only: listen on 127.0.0.1 instead of every interface (no firewall prompt for the offline test host).
void ResponderLoopbackOnly(bool on);

} // namespace h7::net
