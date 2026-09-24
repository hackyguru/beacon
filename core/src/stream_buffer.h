#ifndef BEACON_STREAM_BUFFER_H
#define BEACON_STREAM_BUFFER_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace beacon {

/**
 * Reorders arriving fragments and hands out a byte stream.
 *
 * Gossipsub gives no ordering and no delivery guarantee, so fragments arrive
 * shuffled and sometimes not at all. Fragments are held briefly in sequence
 * order; once the reorder window is full the next fragment is released even if
 * its predecessor never came. A transport stream tolerates that — the decoder
 * resynchronises at the next packet boundary — which is exactly why the loss
 * is survivable here and would not be in a container with a global header.
 *
 * Readers block in @ref read until bytes exist, so the HTTP side can simply
 * stream whatever is ready.
 */
class StreamBuffer
{
public:
    /** `window` fragments of reordering tolerance before a gap is conceded. */
    explicit StreamBuffer(size_t window = 24, size_t maxBytes = 8 * 1024 * 1024);

    /** Feeds a verified fragment in. Out-of-order and duplicates are handled. */
    void push(uint64_t seq, const std::vector<uint8_t>& payload);

    /**
     * Blocks until bytes are available or the buffer is closed.
     * Returns 0 only when closed and drained.
     */
    size_t read(uint8_t* out, size_t max, int timeoutMs = 500);

    /** Drops everything and starts a new sequence — a new station, or a restart. */
    void reset();

    void close();
    void reopen();

    uint64_t fragments() const;
    uint64_t gaps() const;
    uint64_t bytes() const;

private:
    void drainLocked();

    mutable std::mutex      m_mu;
    std::condition_variable m_cv;

    std::map<uint64_t, std::vector<uint8_t>> m_pending;   // seq -> payload
    std::deque<uint8_t>                      m_out;

    uint64_t m_next      = 0;      // next sequence expected
    bool     m_started   = false;
    bool     m_closed    = false;
    uint64_t m_fragments = 0;
    uint64_t m_gaps      = 0;
    uint64_t m_bytes     = 0;

    const size_t m_window;
    const size_t m_maxBytes;
};

} // namespace beacon

#endif // BEACON_STREAM_BUFFER_H
