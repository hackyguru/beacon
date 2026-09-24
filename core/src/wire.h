#ifndef BEACON_WIRE_H
#define BEACON_WIRE_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace beacon {

/**
 * The packet a station puts on its topic.
 *
 *   magic   4  "BCN1"
 *   kind    1  1 = media (MPEG-TS bytes)
 *   pubkey 32  the station's ed25519 public key — its identity
 *   seq     8  monotonic, assigned by the broadcaster
 *   sentMs  8  broadcaster's clock, for latency reporting only
 *   sig    64  ed25519 over everything after it plus the payload
 *   payload    a slice of the transport stream
 *
 * The signature is what makes a broadcast different from a call. Anyone can
 * publish on a public topic, so viewers verify every fragment against the
 * station key they chose to watch and drop the rest: a stranger can waste the
 * mesh's bandwidth, but cannot be heard on someone else's station.
 */
namespace wire {

constexpr uint8_t  kMagic[4]   = { 'B', 'C', 'N', '1' };
constexpr uint8_t  kKindMedia  = 1;
constexpr size_t   kPubKeyLen  = 32;
constexpr size_t   kSigLen     = 64;
constexpr size_t   kHeaderLen  = 4 + 1 + kPubKeyLen + 8 + 8 + kSigLen;   // 117

/** Bytes that the signature covers: seq, sentMs and the payload. */
inline std::vector<uint8_t> signedRegion(uint64_t seq, int64_t sentMs,
                                         const uint8_t* payload, size_t len)
{
    std::vector<uint8_t> out;
    out.reserve(16 + len);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((seq >> (8 * i)) & 0xff));
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(sentMs) >> (8 * i)) & 0xff));
    out.insert(out.end(), payload, payload + len);
    return out;
}

struct Packet {
    uint8_t              pubkey[kPubKeyLen]{};
    uint64_t             seq    = 0;
    int64_t              sentMs = 0;
    std::vector<uint8_t> sig;
    std::vector<uint8_t> payload;
};

inline std::vector<uint8_t> encode(const uint8_t pubkey[kPubKeyLen], uint64_t seq,
                                   int64_t sentMs, const uint8_t sig[kSigLen],
                                   const uint8_t* payload, size_t len)
{
    std::vector<uint8_t> out;
    out.reserve(kHeaderLen + len);
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(kKindMedia);
    out.insert(out.end(), pubkey, pubkey + kPubKeyLen);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((seq >> (8 * i)) & 0xff));
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(sentMs) >> (8 * i)) & 0xff));
    out.insert(out.end(), sig, sig + kSigLen);
    out.insert(out.end(), payload, payload + len);
    return out;
}

inline bool decode(const std::vector<uint8_t>& in, Packet& out)
{
    if (in.size() <= kHeaderLen) return false;
    if (std::memcmp(in.data(), kMagic, 4) != 0) return false;
    if (in[4] != kKindMedia) return false;

    size_t p = 5;
    std::memcpy(out.pubkey, in.data() + p, kPubKeyLen);
    p += kPubKeyLen;

    out.seq = 0;
    for (int i = 0; i < 8; ++i) out.seq |= static_cast<uint64_t>(in[p + i]) << (8 * i);
    p += 8;

    uint64_t ms = 0;
    for (int i = 0; i < 8; ++i) ms |= static_cast<uint64_t>(in[p + i]) << (8 * i);
    out.sentMs = static_cast<int64_t>(ms);
    p += 8;

    out.sig.assign(in.begin() + p, in.begin() + p + kSigLen);
    p += kSigLen;

    out.payload.assign(in.begin() + p, in.end());
    return true;
}

} // namespace wire
} // namespace beacon

#endif // BEACON_WIRE_H
