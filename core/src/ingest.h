#ifndef BEACON_INGEST_H
#define BEACON_INGEST_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace beacon {

/**
 * The loopback port a streamer's own software pushes MPEG-TS at.
 *
 * MPEG-TS rather than RTMP on purpose: it is a stream of self-contained
 * 188-byte packets that repeats its own headers, so it can be cut anywhere,
 * needs no global header, and a viewer arriving mid-broadcast can start
 * decoding at the next keyframe without anything having been negotiated.
 * Beacon never re-encodes — whatever OBS produced is what viewers receive.
 *
 * Bound to 127.0.0.1: a cloud studio cannot reach it, and neither can anyone
 * else on the network. Ingest is local, distribution is peer to peer.
 */
class Ingest
{
public:
    using Sink = std::function<void(const uint8_t* data, size_t len)>;

    ~Ingest();

    /**
     * Binds `preferredPort` (0 for ephemeral) and starts reading. A port that
     * stays the same across broadcasts matters more than it sounds: OBS keeps
     * the URL in its own settings, and re-entering it means reopening a dialog
     * that resets the encoder choice.
     */
    bool start(const Sink& sink, std::string& error, int preferredPort = 0);
    void stop();

    bool     running() const { return m_running; }
    int      port() const { return m_port; }
    /** What to paste into OBS as the output URL. */
    std::string url() const;

    uint64_t bytesIn() const { return m_bytesIn; }
    int64_t  lastPacketMs() const { return m_lastPacketMs; }

private:
    void readLoop(Sink sink);

    std::thread          m_thread;
    std::atomic<bool>    m_running{false};
    std::atomic<int>     m_port{0};
    std::atomic<uint64_t> m_bytesIn{0};
    std::atomic<int64_t> m_lastPacketMs{0};
    int                  m_fd = -1;
};

} // namespace beacon

#endif // BEACON_INGEST_H
