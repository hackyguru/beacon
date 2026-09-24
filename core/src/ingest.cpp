#include "ingest.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace beacon {

namespace {

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

Ingest::~Ingest()
{
    stop();
}

bool Ingest::start(const Sink& sink, std::string& error)
{
    if (m_running) return true;

    m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    // A video stream in bursts overruns the default receive buffer and the
    // kernel drops datagrams silently, which looks like a corrupt stream.
    int rcvbuf = 4 * 1024 * 1024;
    ::setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;                       // ephemeral: two Basecamps must not collide

    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error = std::string("bind: ") + std::strerror(errno);
        ::close(m_fd);
        m_fd = -1;
        return false;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        m_port = ntohs(addr.sin_port);

    m_bytesIn      = 0;
    m_lastPacketMs = 0;
    m_running      = true;
    m_thread       = std::thread([this, sink] { readLoop(sink); });
    return true;
}

void Ingest::stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_fd >= 0) {
        ::shutdown(m_fd, SHUT_RDWR);
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_thread.joinable()) m_thread.join();
    m_port = 0;
}

std::string Ingest::url() const
{
    if (!m_running) return {};
    return "udp://127.0.0.1:" + std::to_string(m_port.load());
}

void Ingest::readLoop(Sink sink)
{
    // ffmpeg's default MPEG-TS datagram is 7 × 188 bytes; leave room for more.
    std::vector<uint8_t> buf(65536);
    while (m_running) {
        const ssize_t n = ::recv(m_fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (!m_running) return;
            continue;
        }
        m_bytesIn += static_cast<uint64_t>(n);
        m_lastPacketMs = nowMs();
        if (sink) sink(buf.data(), static_cast<size_t>(n));
    }
}

} // namespace beacon
