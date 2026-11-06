// SHA-256, HMAC-SHA256 and secure random bytes through Windows CNG (bcrypt.dll).
#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <vector>

namespace h7::net {

static std::string ToHex(const unsigned char* p, size_t n) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = kDigits[p[i] >> 4];
        out[2 * i + 1] = kDigits[p[i] & 15];
    }
    return out;
}

// Algorithm handles are opened once and shared: CNG allows creating hash objects from one handle on many threads.
static BCRYPT_ALG_HANDLE Provider(bool hmac) {
    static BCRYPT_ALG_HANDLE plain = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) h = nullptr;
        return h;
    }();
    static BCRYPT_ALG_HANDLE keyed = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) h = nullptr;
        return h;
    }();
    return hmac ? keyed : plain;
}

static std::string Digest(bool hmac, const std::string& key, const std::string& data) {
    BCRYPT_ALG_HANDLE alg = Provider(hmac);
    if (!alg) return {};
    // CNG wants a non-null secret pointer even for an empty HMAC key.
    static unsigned char empty = 0;
    PUCHAR secret = hmac ? (key.empty() ? &empty : (PUCHAR)key.data()) : nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(alg, &h, nullptr, 0, secret, hmac ? (ULONG)key.size() : 0, 0) < 0) return {};
    unsigned char out[32];
    bool ok = BCryptHashData(h, (PUCHAR)(data.empty() ? (const char*)&empty : data.data()), (ULONG)data.size(), 0) >= 0
        && BCryptFinishHash(h, out, sizeof out, 0) >= 0;
    BCryptDestroyHash(h);
    return ok ? ToHex(out, sizeof out) : std::string();
}

std::string Sha256Hex(const std::string& data) { return Digest(false, {}, data); }

std::string HmacSha256Hex(const std::string& key, const std::string& msg) { return Digest(true, key, msg); }

std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> buf(bytes ? bytes : 1);
    if (BCryptGenRandom(nullptr, buf.data(), (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    return ToHex(buf.data(), bytes);
}

} // namespace h7::net
