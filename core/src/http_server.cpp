#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
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
  #o{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;
     background:rgba(0,0,0,.35)}
  #b{background:#ED7B58;color:#241511;border:0;border-radius:999px;
     padding:14px 26px;font-weight:700;font-size:15px;cursor:pointer}
  #s{position:fixed;left:0;right:0;bottom:0;padding:6px 10px;color:#9a9a9a;
     background:rgba(0,0,0,.45);font-size:11px}
  .hidden{display:none!important}
</style></head><body>
<video id="v" muted playsinline autoplay></video>
<div id="o"><button id="b">Play</button></div>
<div id="s">connecting…</div>
<script src="mpegts.js"></script>
<script>
  var v=document.getElementById('v'), s=document.getElementById('s'),
      o=document.getElementById('o'), b=document.getElementById('b'),
      player=null, lastErr='', started=false;

  function report(){
    var f={};
    try{ f=mpegts.getFeatureList(); }catch(e){}
    var buf=[];
    try{ for(var i=0;i<v.buffered.length;i++) buf.push(v.buffered.start(i).toFixed(1)+'-'+v.buffered.end(i).toFixed(1)); }catch(e){}
    var st={
      mse: !!(window.MediaSource), mseLive: !!f.mseLivePlayback,
      msePlus: !!f.mseLiveFlvPlayback, ready: v.readyState, net: v.networkState,
      paused: v.paused, muted: v.muted, t: +v.currentTime.toFixed(2),
      w: v.videoWidth, h: v.videoHeight, buffered: buf.join(','),
      err: lastErr || (v.error ? ('media '+v.error.code+' '+(v.error.message||'')) : ''),
      ua: navigator.userAgent.slice(0,80)
    };
    try{ fetch('log',{method:'POST',body:JSON.stringify(st)}); }catch(e){}
    s.textContent = (st.w? st.w+'x'+st.h+' ':'') + 'ready='+st.ready+
      (st.paused?' paused':'') + (st.err? ' · '+st.err : '') +
      (st.buffered? ' · buf '+st.buffered : '');
    if(!v.paused && st.ready>=2) o.classList.add('hidden');
  }
  report();
  setInterval(report, 1000);

  function start(){
    var f = mpegts.getFeatureList();
    if(!f.mseLivePlayback){ lastErr='MSE live playback unsupported'; report(); return; }
    player = mpegts.createPlayer(
      { type:'mpegts', isLive:true, url:'live.ts' },
      { enableWorker:false, lazyLoad:false, fixAudioTimestampGap:true,
        autoCleanupSourceBuffer:true,
        liveBufferLatencyChasing:true, liveBufferLatencyMaxLatency:3.0,
        liveBufferLatencyMinRemain:0.8 });
    player.attachMediaElement(v);
    player.on(mpegts.Events.ERROR, function(a,b2,c){
      lastErr = 'stream '+a+'/'+b2; report(); setTimeout(restart, 2000);
    });
    player.load();
    v.play().catch(function(e){ lastErr='autoplay blocked: '+e.name; report(); });
    started=true;
  }
  function restart(){
    try{ if(player){ player.destroy(); player=null; } }catch(e){}
    start();
  }
  v.addEventListener('playing', function(){ lastErr=''; o.classList.add('hidden'); });
  function go(){ v.muted=false; v.play().catch(function(e){ lastErr='play(): '+e.name; }); o.classList.add('hidden'); }
  b.onclick=go; document.body.addEventListener('click', function(){ if(v.paused) go(); });
  start();
</script></body></html>
)HTML";

/** False once the peer has gone away — the caller should stop streaming. */
bool sendAll(int fd, const char* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
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

std::string HttpServer::playerLog() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_playerLog;
}

std::string HttpServer::requestCounts() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    std::string out;
    for (const auto& kv : m_hits) {
        if (!out.empty()) out += " ";
        out += kv.first + "=" + std::to_string(kv.second);
    }
    return out;
}

void HttpServer::handleClient(int fd)
{
    char buf[8192];
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';
    const std::string req(buf);

    // The player page reports what it sees — codec support, readyState, errors
    // — so a black picture can be diagnosed without a devtools window, which a
    // WebView inside a plugin does not have.
    if (req.compare(0, 5, "POST ") == 0) {
        const size_t bodyAt = req.find("\r\n\r\n");
        if (bodyAt != std::string::npos) {
            std::string body = req.substr(bodyAt + 4);

            // fetch() sends the headers and the body in separate segments, so
            // the first read usually stops at the blank line. Keep reading
            // until Content-Length is satisfied or the peer stops talking.
            size_t want = 0;
            const size_t clAt = req.find("Content-Length:");
            if (clAt != std::string::npos)
                want = static_cast<size_t>(std::strtoul(req.c_str() + clAt + 15, nullptr, 10));
            while (body.size() < want) {
                const ssize_t more = ::recv(fd, buf, sizeof(buf) - 1, 0);
                if (more <= 0) break;
                buf[more] = '\0';
                body.append(buf, static_cast<size_t>(more));
            }

            std::lock_guard<std::mutex> lk(m_mu);
            m_playerLog = body;
        }
        sendSimple(fd, "200 OK", "text/plain", "ok");
        return;
    }
    if (req.compare(0, 4, "GET ") != 0) return;

    const size_t sp = req.find(' ', 4);
    std::string path = req.substr(4, sp - 4);
    const size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    {
        std::lock_guard<std::mutex> lk(m_mu);
        ++m_hits[path];
    }

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
    // Each player gets its own view of the stream: two of them (a reconnect
    // overlapping its predecessor, say) must not eat each other's bytes.
    auto reader = src->addReader();
    std::vector<uint8_t> chunk(32 * 1024);
    while (m_running) {
        const size_t got = src->read(reader, chunk.data(), chunk.size(), 500);

        // A player that closed its tab has to be noticed even when no video is
        // flowing — otherwise every retry leaves a thread and a queue behind,
        // and they accumulate in their hundreds while a station is off air.
        char probe[1];
        const ssize_t peek = ::recv(fd, probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (peek == 0) break;

        if (got == 0) {
            // Keep the connection open through a quiet stretch; a zero-length
            // chunk would end the response.
            continue;
        }
        char sizeLine[32];
        const int sl = std::snprintf(sizeLine, sizeof(sizeLine), "%zx\r\n", got);
        // A closed tab shows up as a failed send. Without acting on it the
        // thread, and this reader's copy of the stream, live forever.
        if (!sendAll(fd, sizeLine, static_cast<size_t>(sl)) ||
            !sendAll(fd, reinterpret_cast<const char*>(chunk.data()), got) ||
            !sendAll(fd, "\r\n", 2))
            break;

        // Detect a viewer who closed the tab: a send to a dead socket fails and
        // ::send has already returned early, so probe the peer.
    }
    src->removeReader(reader);
    --m_liveClients;
}

} // namespace beacon
