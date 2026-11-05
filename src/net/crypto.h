#pragma once
#include <string>

namespace h7::net {

// Windows CNG (BCrypt), no extra dependency. Thread-safe. Hex output is lowercase; "" only if CNG fails.
std::string Sha256Hex(const std::string& data);
std::string HmacSha256Hex(const std::string& key, const std::string& msg);
std::string RandomHex(size_t bytes); // cryptographically secure (system preferred RNG): 2 * bytes hex chars

} // namespace h7::net
