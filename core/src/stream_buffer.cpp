#include "stream_buffer.h"

#include <algorithm>
#include <chrono>

namespace beacon {

StreamBuffer::StreamBuffer(size_t window, size_t maxBytes)
    : m_window(window), m_maxBytes(maxBytes)
{
}

void StreamBuffer::push(uint64_t seq, const std::vector<uint8_t>& payload)
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_closed) return;

        // First fragment seen sets the origin: a viewer joins mid-broadcast and
        // must not wait for sequence 0 that was sent an hour ago.
        if (!m_started) {
            m_started = true;
            m_next    = seq;
        }
        if (seq < m_next) return;              // late or duplicate: already conceded
        m_pending[seq] = payload;
        ++m_fragments;
        drainLocked();
    }
    m_cv.notify_all();
}

void StreamBuffer::drainLocked()
{
    // Release everything contiguous from m_next.
    for (auto it = m_pending.find(m_next); it != m_pending.end(); it = m_pending.find(m_next)) {
        m_out.insert(m_out.end(), it->second.begin(), it->second.end());
        m_bytes += it->second.size();
        m_pending.erase(it);
        ++m_next;
    }

    // Still stuck behind a missing fragment with a full window: concede the gap
    // and jump to the oldest fragment we do hold.
    while (m_pending.size() > m_window) {
        const uint64_t oldest = m_pending.begin()->first;
        m_gaps += (oldest > m_next) ? (oldest - m_next) : 1;
        m_next = oldest;
        for (auto it = m_pending.find(m_next); it != m_pending.end(); it = m_pending.find(m_next)) {
            m_out.insert(m_out.end(), it->second.begin(), it->second.end());
            m_bytes += it->second.size();
            m_pending.erase(it);
            ++m_next;
        }
    }

    // A viewer whose player stalled must not grow the buffer without bound;
    // drop from the front, which is the oldest and least useful video.
    while (m_out.size() > m_maxBytes)
        m_out.pop_front();
}

size_t StreamBuffer::read(uint8_t* out, size_t max, int timeoutMs)
{
    std::unique_lock<std::mutex> lk(m_mu);
    if (m_out.empty() && !m_closed)
        m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return !m_out.empty() || m_closed; });

    const size_t n = std::min(max, m_out.size());
    for (size_t i = 0; i < n; ++i) {
        out[i] = m_out.front();
        m_out.pop_front();
    }
    return n;
}

void StreamBuffer::reset()
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_pending.clear();
        m_out.clear();
        m_next    = 0;
        m_started = false;
        m_fragments = m_gaps = m_bytes = 0;
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

} // namespace beacon
