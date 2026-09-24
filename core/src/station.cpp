#include "station.h"

#include <sodium.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace beacon {

namespace {

bool sodiumReady()
{
    static const bool ok = (sodium_init() >= 0);
    return ok;
}

} // namespace

std::string Station::toHex(const uint8_t* bytes, size_t len)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0f]);
    }
    return out;
}

bool Station::hexToKey(const std::string& hex, uint8_t out[32])
{
    if (hex.size() != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        unsigned v = 0;
        if (std::sscanf(hex.c_str() + i * 2, "%2x", &v) != 1) return false;
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

bool Station::loadOrCreate(const std::string& dir, std::string& error)
{
    if (!sodiumReady()) {
        error = "libsodium failed to initialise";
        return false;
    }
    const std::string path = dir + "/station.key";

    std::ifstream in(path, std::ios::binary);
    if (in) {
        in.read(reinterpret_cast<char*>(m_pk), sizeof(m_pk));
        in.read(reinterpret_cast<char*>(m_sk), sizeof(m_sk));
        if (in.gcount() == sizeof(m_sk)) {
            m_haveKeys = m_canSign = true;
            return true;
        }
        error = "station.key is truncated";
        return false;
    }

    crypto_sign_keypair(m_pk, m_sk);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(m_pk), sizeof(m_pk));
    out.write(reinterpret_cast<const char*>(m_sk), sizeof(m_sk));
    out.close();
    // The secret key is the station's identity — nobody else on the machine
    // needs to read it.
    ::chmod(path.c_str(), 0600);

    m_haveKeys = m_canSign = true;
    return true;
}

std::string Station::exportSecretHex() const
{
    if (!m_canSign) return {};
    return toHex(m_sk, sizeof(m_sk));
}

bool Station::importSecretHex(const std::string& dir, const std::string& hex, std::string& error)
{
    if (!sodiumReady()) {
        error = "libsodium failed to initialise";
        return false;
    }
    if (hex.size() != sizeof(m_sk) * 2) {
        error = "a station backup is 128 hex characters";
        return false;
    }
    uint8_t sk[64];
    for (size_t i = 0; i < sizeof(sk); ++i) {
        unsigned v = 0;
        if (std::sscanf(hex.c_str() + i * 2, "%2x", &v) != 1) {
            error = "that is not hex";
            return false;
        }
        sk[i] = static_cast<uint8_t>(v);
    }
    // An ed25519 secret key carries its public half in the last 32 bytes;
    // derive rather than trust, so a malformed pair is caught here.
    uint8_t pk[32];
    if (crypto_sign_ed25519_sk_to_pk(pk, sk) != 0) {
        error = "that is not a station key";
        return false;
    }

    std::memcpy(m_sk, sk, sizeof(m_sk));
    std::memcpy(m_pk, pk, sizeof(m_pk));
    m_haveKeys = m_canSign = true;

    const std::string path = dir + "/station.key";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(m_pk), sizeof(m_pk));
    out.write(reinterpret_cast<const char*>(m_sk), sizeof(m_sk));
    out.close();
    ::chmod(path.c_str(), 0600);
    return true;
}

void Station::createEphemeral()
{
    if (!sodiumReady()) return;
    crypto_sign_keypair(m_pk, m_sk);
    m_haveKeys = m_canSign = true;
}

std::string Station::publicKeyHex() const
{
    return m_haveKeys ? toHex(m_pk, sizeof(m_pk)) : std::string();
}

std::string Station::shortId() const
{
    const std::string hex = publicKeyHex();
    return hex.size() >= 8 ? hex.substr(0, 8) : hex;
}

std::vector<uint8_t> Station::sign(const std::vector<uint8_t>& data) const
{
    if (!m_canSign || !sodiumReady()) return {};
    std::vector<uint8_t> sig(crypto_sign_BYTES);
    unsigned long long len = 0;
    if (crypto_sign_detached(sig.data(), &len, data.data(), data.size(), m_sk) != 0)
        return {};
    sig.resize(static_cast<size_t>(len));
    return sig;
}

bool Station::verify(const uint8_t* pubkey, const std::vector<uint8_t>& sig,
                     const std::vector<uint8_t>& data)
{
    if (!sodiumReady() || sig.size() != crypto_sign_BYTES) return false;
    return crypto_sign_verify_detached(sig.data(), data.data(), data.size(), pubkey) == 0;
}

} // namespace beacon
