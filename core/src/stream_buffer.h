#ifndef BEACON_STREAM_BUFFER_H
#define BEACON_STREAM_BUFFER_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace beacon {

/**
 * Reorders arriving fragments and fans the result out to readers.
 *
 * Gossipsub gives no ordering and no delivery guarantee, so fragments arrive
 * shuffled and sometimes not at all. Fragments are held briefly in sequence
 * order; once the reorder window is full the next one is released even if its
 * predecessor never came. A transport stream tolerates that — the decoder
 * resynchronises at the next packet boundary.
 *
 * Two rules keep the picture stable:
 *
 *  - Readers are independent. Bytes handed to one are not consumed from
 *    another, so a reconnecting player never starves the one still running.
 *  - Nothing is ever cut mid-fragment. Fragments are whole multiples of the
 *    188-byte transport packet, so dropping one costs a glitch; dropping half
 *    of one desynchronises the decoder and the picture flickers until it
 *    recovers.
 *
 * With no reader attached the stream is discarded rather than buffered: a
 * viewer that arrives later wants live video, not a minute of stale frames.
 */
class StreamBuffer
{
public:
    struct Reader;
    using ReaderHandle = std::shared_ptr<Reader>;

    explicit StreamBuffer(size_t window = 24, size_t maxFragmentsPerReader = 512);

    /** Feeds a verified fragment in. Out-of-order and duplicates are handled. */
    void push(uint64_t seq, const std::vector<uint8_t>& payload);

    /** A player connecting. Release it with @ref removeReader. */
    ReaderHandle addReader();
    void removeReader(const ReaderHandle& reader);
    int  readerCount() const;

    /** Blocks until bytes exist for this reader, or the timeout passes. */
    size_t read(const ReaderHandle& reader, uint8_t* out, size_t max, int timeoutMs = 500);

    /** Drops everything and starts a new sequence — a new station, or a restart. */
    void reset();
    void close();
    void reopen();

    uint64_t fragments() const;
    uint64_t gaps() const;
    uint64_t bytes() const;
    uint64_t dropped() const;

private:
    void drainLocked();
    void emitLocked(const std::vector<uint8_t>& payload);

    mutable std::mutex      m_mu;
    std::condition_variable m_cv;

    std::map<uint64_t, std::vector<uint8_t>> m_pending;    // seq -> payload
    std::vector<ReaderHandle>                m_readers;

    uint64_t m_next      = 0;
    bool     m_started   = false;
    bool     m_closed    = false;
    uint64_t m_fragments = 0;
    uint64_t m_gaps      = 0;
    uint64_t m_bytes     = 0;
    uint64_t m_dropped   = 0;      // fragments dropped because a reader fell behind

    const size_t m_window;
    const size_t m_maxFragments;
};

/** One player's view of the stream: whole fragments, consumed in order. */
struct StreamBuffer::Reader {
    std::deque<std::vector<uint8_t>> queue;
    size_t offset = 0;             // how far into queue.front() this reader has read
};

} // namespace beacon

#endif // BEACON_STREAM_BUFFER_H
