// Offline checks on the parts a broadcast's integrity rests on: a viewer must
// accept fragments from the station it asked for, and nothing else. No audio
// device, no network, no Basecamp — this compiles and runs on its own.
//
//   core/tests/run.sh

#include "../src/station.h"
#include "../src/stream_buffer.h"
#include "../src/wire.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace beacon;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++failures;
}

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

constexpr int64_t kMaxAgeMs = 30000;    // must match beacon_core_impl.cpp

std::vector<uint8_t> payload(size_t n, uint8_t fill)
{
    return std::vector<uint8_t>(n, fill);
}

/** What the viewer does with an arriving packet, minus the plumbing. */
bool viewerAccepts(const Station& expected, const std::string& topic,
                   const std::vector<uint8_t>& packet, int64_t now)
{
    wire::Packet pkt;
    if (!wire::decode(packet, pkt)) return false;
    if (std::memcmp(pkt.pubkey, expected.publicKey(), wire::kPubKeyLen) != 0) return false;

    const auto signed_ = wire::signedRegion(topic, pkt.seq, pkt.sentMs,
                                            pkt.payload.data(), pkt.payload.size());
    if (!Station::verify(expected.publicKey(), pkt.sig, signed_)) return false;

    const int64_t age = now - pkt.sentMs;
    return age <= kMaxAgeMs && age >= -kMaxAgeMs;
}

std::vector<uint8_t> publish(const Station& s, const std::string& topic, uint64_t seq,
                             int64_t sentMs, const std::vector<uint8_t>& body)
{
    const auto signed_ = wire::signedRegion(topic, seq, sentMs, body.data(), body.size());
    const auto sig     = s.sign(signed_);
    return wire::encode(s.publicKey(), seq, sentMs, sig.data(), body.data(), body.size());
}

} // namespace

int main()
{
    Station broadcaster, impostor;
    broadcaster.createEphemeral();
    impostor.createEphemeral();

    const std::string topic  = "/beacon/1/" + broadcaster.publicKeyHex() + "/ts";
    const std::string other  = "/beacon/1/" + impostor.publicKeyHex() + "/ts";
    const int64_t     now    = nowMs();
    const auto        body   = payload(188 * 4, 0x47);

    std::puts("station identity");
    check(viewerAccepts(broadcaster, topic, publish(broadcaster, topic, 1, now, body), now),
          "a fragment from the station is accepted");
    check(!viewerAccepts(broadcaster, topic, publish(impostor, topic, 1, now, body), now),
          "a fragment signed by someone else on the same topic is rejected");

    std::puts("\ntampering");
    {
        auto packet = publish(broadcaster, topic, 1, now, body);
        packet[wire::kHeaderLen + 10] ^= 0xff;                       // flip a payload bit
        check(!viewerAccepts(broadcaster, topic, packet, now), "altered video is rejected");
    }
    {
        auto packet = publish(broadcaster, topic, 1, now, body);
        packet[5 + wire::kPubKeyLen] ^= 0x01;                        // bump the sequence
        check(!viewerAccepts(broadcaster, topic, packet, now), "an altered sequence number is rejected");
    }
    {
        // The station's own fragment, lifted onto another station's topic.
        const auto packet = publish(broadcaster, topic, 1, now, body);
        check(!viewerAccepts(broadcaster, other, packet, now),
              "a fragment replayed onto another topic is rejected");
    }

    std::puts("\nreplay");
    {
        // Genuinely signed by the station — just not today.
        const auto old = publish(broadcaster, topic, 999999, now - 3600 * 1000, body);
        check(!viewerAccepts(broadcaster, topic, old, now),
              "yesterday's broadcast replayed as live is rejected");
        const auto recent = publish(broadcaster, topic, 999999, now - 5000, body);
        check(viewerAccepts(broadcaster, topic, recent, now),
              "a fragment a few seconds old still plays");
        const auto future = publish(broadcaster, topic, 1, now + 10 * 60 * 1000, body);
        check(!viewerAccepts(broadcaster, topic, future, now),
              "a fragment from the future is rejected");
    }

    std::puts("\nreordering");
    {
        StreamBuffer buf(4);
        auto reader = buf.addReader();
        buf.push(10, payload(188, 0xa1));
        buf.push(12, payload(188, 0xa3));
        buf.push(11, payload(188, 0xa2));
        std::vector<uint8_t> out(188 * 4);
        const size_t got = buf.read(reader, out.data(), out.size(), 100);
        check(got == 188 * 3, "out-of-order fragments are released in sequence");
        check(out[0] == 0xa1 && out[188] == 0xa2 && out[376] == 0xa3, "and in the right order");

        buf.push(9, payload(188, 0xff));
        const size_t late = buf.read(reader, out.data(), out.size(), 100);
        check(late == 0, "a fragment that arrives after its turn is dropped");
    }
    {
        StreamBuffer buf(2);
        auto reader = buf.addReader();
        buf.push(1, payload(188, 0x01));
        for (uint64_t s = 5; s < 12; ++s) buf.push(s, payload(188, 0x05));   // 2..4 never arrive
        std::vector<uint8_t> out(188 * 16);
        const size_t got = buf.read(reader, out.data(), out.size(), 100);
        check(got > 188, "a missing fragment is conceded rather than stalling playback");
        check(buf.gaps() > 0, "and the gap is counted");
    }
    {
        StreamBuffer buf(4);
        auto a = buf.addReader();
        auto b = buf.addReader();
        buf.push(1, payload(188, 0x42));
        std::vector<uint8_t> oa(188), ob(188);
        check(buf.read(a, oa.data(), oa.size(), 100) == 188 &&
              buf.read(b, ob.data(), ob.size(), 100) == 188,
              "two players each get the whole stream");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all good");
    return failures ? 1 : 0;
}
