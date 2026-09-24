#include "stream_buffer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace beacon {

StreamBuffer::StreamBuffer(size_t window, size_t maxFragmentsPerReader)
    : m_window(window), m_maxFragments(maxFragmentsPerReader)
{
}

StreamBuffer::ReaderHandle StreamBuffer::addReader()
{
    auto reader = std::make_shared<Reader>();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_readers.push_back(reader);
    }
    return reader;
}

void StreamBuffer::removeReader(const ReaderHandle& reader)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_readers.erase(std::remove(m_readers.begin(), m_readers.end(), reader), m_readers.end());
}

int StreamBuffer::readerCount() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return static_cast<int>(m_readers.size());
}

void StreamBuffer::push(uint64_t seq, const std::vector<uint8_t>& payload)
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_closed) return;

        // The first fragment seen sets the origin: a viewer joining mid-stream
        // must not wait for a sequence sent an hour ago.
        if (!m_started) {
            m_started = true;
            m_next    = seq;
        }
        if (seq < m_next) return;            // late or duplicate: already conceded
        m_pending[seq] = payload;
        ++m_fragments;
        drainLocked();
    }
    m_cv.notify_all();
}

void StreamBuffer::emitLocked(const std::vector<uint8_t>& payload)
{
    m_bytes += payload.size();
    for (auto& r : m_readers) {
        r->queue.push_back(payload);
        // A player that stalled must not grow without bound. Drop whole
        // fragments from the front — never part of one, which would leave the
        // decoder mid-packet.
        while (r->queue.size() > m_maxFragments) {
            if (r->queue.size() == 1) break;
            r->queue.pop_front();
            r->offset = 0;
            ++m_dropped;
        }
    }
}

void StreamBuffer::drainLocked()
{
    for (auto it = m_pending.find(m_next); it != m_pending.end(); it = m_pending.find(m_next)) {
        emitLocked(it->second);
        m_pending.erase(it);
        ++m_next;
    }

    // Still stuck behind a missing fragment with a full window: concede the gap
    // and continue from the oldest fragment held.
    while (m_pending.size() > m_window) {
        const uint64_t oldest = m_pending.begin()->first;
        m_gaps += (oldest > m_next) ? (oldest - m_next) : 1;
        m_next = oldest;
        for (auto it = m_pending.find(m_next); it != m_pending.end(); it = m_pending.find(m_next)) {
            emitLocked(it->second);
            m_pending.erase(it);
            ++m_next;
        }
    }
}

size_t StreamBuffer::read(const ReaderHandle& reader, uint8_t* out, size_t max, int timeoutMs)
{
    if (!reader) return 0;

    std::unique_lock<std::mutex> lk(m_mu);
    if (reader->queue.empty() && !m_closed)
        m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                      [&] { return !reader->queue.empty() || m_closed; });

    size_t written = 0;
    while (written < max && !reader->queue.empty()) {
        const auto& front = reader->queue.front();
        const size_t have = front.size() - reader->offset;
        const size_t take = std::min(have, max - written);
        std::memcpy(out + written, front.data() + reader->offset, take);
        written        += take;
        reader->offset += take;
        if (reader->offset >= front.size()) {
            reader->queue.pop_front();
            reader->offset = 0;
        }
    }
    return written;
}

void StreamBuffer::reset()
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_pending.clear();
        for (auto& r : m_readers) {
            r->queue.clear();
            r->offset = 0;
        }
        m_next    = 0;
        m_started = false;
        m_fragments = m_gaps = m_bytes = m_dropped = 0;
    }
    m_cv.notify_all();
}

void StreamBuffer::close()
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_closed = true;
    }
    m_cv.notify_all();
}

void StreamBuffer::reopen()
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_closed = false;
}

uint64_t StreamBuffer::fragments() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_fragments;
}

uint64_t StreamBuffer::gaps() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_gaps;
}

uint64_t StreamBuffer::bytes() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_bytes;
}

uint64_t StreamBuffer::dropped() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_dropped;
}

} // namespace beacon
