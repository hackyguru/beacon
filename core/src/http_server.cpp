#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "mpegts_js.h"
#include "stream_buffer.h"

namespace beacon {

namespace {

constexpr int kBacklog = 8;

// Autoplay with sound is refused without a gesture, so the player starts muted
// and offers one tap to unmute. Everything else is mpegts.js defaults tuned for
// live: no seeking, small buffer, and a reload if the stream stalls.
const char* const kPlayerHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<style>
  html,body{margin:0;height:100%;background:#000;overflow:hidden;
            font:13px -apple-system,system-ui,sans-serif;color:#EDEDED}
  #v{width:100%;height:100%;object-fit:contain;background:#000}
  #o{position:fixed;inset:0;display:flex;align-items:center;justify-content:center}
  #b{background:#ED7B58;color:#241511;border:0;border-radius:999px;
     padding:12px 22px;font-weight:700;font-size:14px;cursor:pointer}
  #s{position:fixed;left:0;right:0;bottom:0;padding:6px 10px;color:#9a9a9a;
     background:rgba(0,0,0,.45);font-size:11px}
  .hidden{display:none!important}
</style></head><body>
<video id="v" muted playsinline autoplay></video>
<div id="o"><button id="b">Tap for sound</button></div>
<div id="s">connecting…</div>
<script src="mpegts.js"></script>
<script>
  var v=document.getElementById('v'), s=document.getElementById('s'),
      o=document.getElementById('o'), b=document.getElementById('b'), player=null;
  function status(t){ s.textContent=t; }
  function start(){
    if(!mpegts.getFeatureList().mseLivePlayback){ status('this WebView cannot play live streams'); return; }
    player = mpegts.createPlayer(
      { type:'mpegts', isLive:true, url:'live.ts' },
      { enableWorker:false, liveBufferLatencyChasing:true, lazyLoad:false,
        fixAudioTimestampGap:true, autoCleanupSourceBuffer:true });
    player.attachMediaElement(v);
    player.on(mpegts.Events.ERROR, function(a,b2,c){
      status('stream error: '+a+' '+b2); setTimeout(restart, 1500);
    });
    player.load();
    v.play().catch(function(){});
    status('waiting for video…');
  }
  function restart(){
    try{ if(player){ player.destroy(); player=null; } }catch(e){}
    start();
  }
  v.addEventListener('playing', function(){ status(''); });
  b.onclick=function(){ v.muted=false; v.play(); o.classList.add('hidden'); };
  start();
</script></body></html>
)HTML";

void sendAll(int fd, const char* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

void sendSimple(int fd, const char* status, const char* type, const std::string& body)
{
    const std::string head = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type
        + "\r\nCache-Control: no-store\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n";
    sendAll(fd, head.c_str(), head.size());
    sendAll(fd, body.c_str(), body.size());
}

} // namespace

HttpServer::HttpServer() = default;

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::setSource(StreamBuffer* buffer)
{
    m_source = buffer;
}

void HttpServer::setStatsProvider(std::function<std::string()> fn)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_stats = std::move(fn);
}

bool HttpServer::start()
{
    if (m_running) return true;

    m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_error = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    int one = 1;
    ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(m_listenFd, kBacklog) < 0) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_error = std::string("bind/listen: ") + std::strerror(errno);
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(m_listenFd, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        m_port = ntohs(addr.sin_port);

    m_running = true;
    m_thread  = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_listenFd >= 0) {
        ::shutdown(m_listenFd, SHUT_RDWR);
        ::close(m_listenFd);
        m_listenFd = -1;
    }
    if (m_thread.joinable()) m_thread.join();
    m_port = 0;
}

std::string HttpServer::error() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_error;
}

std::string HttpServer::playerUrl() const
{
    if (!m_running) return {};
    return "http://127.0.0.1:" + std::to_string(m_port.load()) + "/";
}

void HttpServer::acceptLoop()
{
    while (m_running) {
        const int fd = ::accept(m_listenFd, nullptr, nullptr);
        if (fd < 0) {
            if (!m_running) return;
            continue;
        }
        // /live.ts never returns while someone is watching, so every connection
        // gets its own thread rather than blocking the accept loop.
        std::thread([this, fd] {
            handleClient(fd);
            ::close(fd);
        }).detach();
    }
}

void HttpServer::handleClient(int fd)
{
    char buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';
    const std::string req(buf);
    if (req.compare(0, 4, "GET ") != 0) return;

    const size_t sp = req.find(' ', 4);
    std::string path = req.substr(4, sp - 4);
    const size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    if (path == "/" || path == "/index.html") {
        sendSimple(fd, "200 OK", "text/html; charset=utf-8", kPlayerHtml);
    } else if (path == "/mpegts.js") {
        sendSimple(fd, "200 OK", "application/javascript; charset=utf-8", kMpegtsJs);
    } else if (path == "/health") {
        sendSimple(fd, "200 OK", "text/plain", "ok");
    } else if (path == "/stats") {
        std::function<std::string()> fn;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            fn = m_stats;
        }
        sendSimple(fd, "200 OK", "application/json", fn ? fn() : std::string("{}"));
    } else if (path == "/live.ts") {
        serveLive(fd);
    } else {
        sendSimple(fd, "404 Not Found", "text/plain", "not found");
    }
}

void HttpServer::serveLive(int fd)
{
    StreamBuffer* src = m_source.load();
    if (!src) {
        sendSimple(fd, "503 Service Unavailable", "text/plain", "no stream");
        return;
    }

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Chunked, never-ending: mpegts.js reads this as a live feed.
    const std::string head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: video/mp2t\r\n"
        "Cache-Control: no-store\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n";
    sendAll(fd, head.c_str(), head.size());

    ++m_liveClients;
    const uint64_t gen = ++m_liveGen;
    std::vector<uint8_t> chunk(32 * 1024);
    while (m_running && m_liveGen == gen) {
        const size_t got = src->read(chunk.data(), chunk.size(), 500);
        if (got == 0) {
            // Keep the connection alive through a quiet stretch; a zero-length
            // chunk would end the response.
            continue;
        }
        char sizeLine[32];
        const int sl = std::snprintf(sizeLine, sizeof(sizeLine), "%zx\r\n", got);
        sendAll(fd, sizeLine, static_cast<size_t>(sl));
        sendAll(fd, reinterpret_cast<const char*>(chunk.data()), got);
        sendAll(fd, "\r\n", 2);

        // Detect a viewer who closed the tab: a send to a dead socket fails and
        // ::send has already returned early, so probe the peer.
        char probe[1];
        const ssize_t r = ::recv(fd, probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0) break;
    }
    --m_liveClients;
}

} // namespace beacon
