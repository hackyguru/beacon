#ifndef BEACON_HTTP_SERVER_H
#define BEACON_HTTP_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace beacon {

class StreamBuffer;

/**
 * A loopback HTTP server: the seam between the module and the WebView.
 *
 * A QML plugin has no video sink — Basecamp ships no Qt Multimedia — so the
 * viewer hands the stream to the WebView instead, and the system's web engine
 * does the decoding, rendering and A/V sync. That only works over HTTP, hence
 * this. Three routes:
 *
 *   /            the player page (mpegts.js attached to a <video>)
 *   /mpegts.js   the embedded library
 *   /live.ts     the live transport stream, chunked, held open
 *
 * Bound to 127.0.0.1 only. Each connection is served on its own thread because
 * /live.ts never returns while a viewer is watching.
 */
class HttpServer
{
public:
    HttpServer();
    ~HttpServer();

    /** Starts on an ephemeral port. Returns false and sets error() on failure. */
    bool start();
    void stop();

    /** Where /live.ts reads from. Not owned; must outlive the server. */
    void setSource(StreamBuffer* buffer);

    /** JSON served at /stats — how the module reports itself without the UI. */
    void setStatsProvider(std::function<std::string()> fn);

    /** The player page posts its own state to /log; this is the last one. */
    std::string playerLog() const;

    /** How many times each route was requested — did the WebView load at all? */
    std::string requestCounts() const;

    int  port() const { return m_port; }
    bool running() const { return m_running; }
    std::string error() const;

    /** The page to point a WebView at — empty while stopped. */
    std::string playerUrl() const;

private:
    void acceptLoop();
    void handleClient(int fd);
    void serveLive(int fd);

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<int>  m_port{0};
    std::atomic<int>  m_liveClients{0};
    int               m_listenFd = -1;

    std::atomic<StreamBuffer*> m_source{nullptr};

    std::function<std::string()> m_stats;

    mutable std::mutex m_mu;
    std::string        m_error;
    std::string        m_playerLog;
    std::map<std::string, uint64_t> m_hits;
};

} // namespace beacon

#endif // BEACON_HTTP_SERVER_H
