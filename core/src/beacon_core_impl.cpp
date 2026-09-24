#include "beacon_core_impl.h"

#include "logos_sdk.h"          // generated: modules().delivery_module
#include "wire.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <sstream>

using nlohmann::json;

namespace {

constexpr const char* kDirectoryTopic = "/beacon/1/directory/json";

// A fragment is a slice of transport stream. 8 KiB is 44 TS packets: small
// enough that one lost fragment is a blip rather than a stall, large enough
// that a 700 kbit/s stream costs about eleven messages a second. The 150 KiB
// transport cap is far above it.
constexpr size_t  kFragmentBytes  = 188 * 44;
constexpr int64_t kFlushMs        = 120;     // ship a partial fragment rather than sit on it
constexpr int64_t kAnnounceMs     = 3000;    // how often a live station says it exists
constexpr int64_t kListingTtlMs   = 12000;   // a station unheard from this long is gone

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

BeaconCoreImpl::BeaconCoreImpl()
    : m_http(new beacon::HttpServer)
    , m_buffer(new beacon::StreamBuffer)
    , m_ingest(new beacon::Ingest)
{
}

BeaconCoreImpl::~BeaconCoreImpl()
{
    m_pumping = false;
    if (m_pump.joinable()) m_pump.join();
    if (m_ingest) m_ingest->stop();
    if (m_buffer) m_buffer->close();
    if (m_http) m_http->stop();
}

void BeaconCoreImpl::onContextReady()
{
    const std::string& dir = instancePersistencePath();
    std::string err;
    if (!dir.empty()) {
        // Two Basecamps on one machine share a user directory, so they would
        // otherwise share a station key and a "viewer" would be watching
        // itself. The test hook that gives the second instance its own port
        // gives it its own identity too.
        const char* portEnv = std::getenv("BEACON_TCPPORT");
        const std::string keyDir = portEnv && *portEnv ? (dir + "/" + portEnv) : dir;
        if (keyDir != dir) ::mkdir(keyDir.c_str(), 0700);
        if (!m_station.loadOrCreate(keyDir, err)) {
            std::lock_guard<std::mutex> lk(m_mu);
            m_lastError = err;
        }
    } else {
        // No persistence (tests, or a host that provisions none): a station that
        // lasts as long as the process is better than none at all.
        m_station.createEphemeral();
    }

    m_http->setSource(m_buffer.get());
    m_http->setStatsProvider([this] { return state(); });
    m_http->start();

    m_pumping = true;
    m_pump    = std::thread([this] { pumpLoop(); });

    // Test hooks. Two Basecamps on one machine share a process name and the
    // accessibility layer cannot tell them apart, so driving both UIs from a
    // script does not work; these let a run be scripted end to end.
    const char* autoBroadcast = std::getenv("BEACON_AUTOBROADCAST");
    const char* autoWatch     = std::getenv("BEACON_AUTOWATCH");
    if (autoBroadcast || autoWatch) {
        const std::string title   = autoBroadcast ? autoBroadcast : "";
        const std::string station = autoWatch ? autoWatch : "";
        std::thread([this, title, station] {
            // The dependency is not necessarily ready the moment the context
            // lands, and the node takes a few seconds to find peers.
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!station.empty()) watch(station);
            else                  startBroadcast(title);
        }).detach();
    }
}

std::string BeaconCoreImpl::stationKey()
{
    return m_station.publicKeyHex();
}

std::string BeaconCoreImpl::mediaTopic(const std::string& stationHex)
{
    return "/beacon/1/" + stationHex + "/ts";
}

// ── network ──────────────────────────────────────────────────────────────

void BeaconCoreImpl::wireDeliveryEvents()
{
    if (m_eventsWired) return;
    m_eventsWired = true;

    modules().delivery_module.onMessageReceived(
        [this](const std::string&, const std::string& contentTopic,
               const std::vector<uint8_t>& payload, int64_t) {
            onMessage(contentTopic, payload);
        });

    modules().delivery_module.onConnectionStateChanged(
        [this](const std::string& status, int64_t) {
            std::lock_guard<std::mutex> lk(m_mu);
            m_netStatus = status.find("Connected") != std::string::npos ? 2 : 1;
        });
}

StdLogosResult BeaconCoreImpl::startNetwork()
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_started) return {true, "already running"};
        m_netStatus = 1;
        m_netError.clear();
    }

    if (!m_nodeCreated) {
        // Two Basecamps on one machine collide on the delivery port, so the
        // second instance is told a different one and the pair dial each other
        // over loopback rather than trusting the public fleet to introduce them.
        const char* portEnv = std::getenv("BEACON_TCPPORT");
        const int customPort = portEnv ? std::atoi(portEnv) : 0;
        const bool isB    = customPort > 0;
        const int tcpPort = isB ? customPort : 60000;
        const int udpPort = isB ? 9000 + (tcpPort - 60000) : 9000;

        static const char* KEY_A = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
        static const char* KEY_B = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f21";
        static const char* PEERID_A = "16Uiu2HAm4Ms862Gnqafssgvik4JJ1LuqWMcKNipq4nm2UaoLRbeP";
        static const char* PEERID_B = "16Uiu2HAmKCaJ7sfcm1aHY8TAShdsKttUHvDwTPbgTkJhLQu8AMiG";

        std::ostringstream peer;
        peer << "/ip4/127.0.0.1/tcp/" << (isB ? 60000 : 60001) << "/p2p/"
             << (isB ? PEERID_A : PEERID_B);

        json cfg;
        cfg["logLevel"]      = "INFO";
        cfg["mode"]          = "Core";
        cfg["preset"]        = "logos.dev";
        // The logos.dev fleet moved to cluster 3 (logos-delivery #4113) while
        // delivery_module 0.2.0's preset still says 2, and every fleet peer
        // drops a mismatched node. An explicit clusterId wins over the preset.
        cfg["clusterId"]     = 3;
        cfg["relay"]         = true;
        cfg["tcpPort"]       = tcpPort;
        cfg["discv5UdpPort"] = udpPort;
        cfg["nodeKey"]       = isB ? KEY_B : KEY_A;
        cfg["staticNodes"]   = json::array({ peer.str() });

        const StdLogosResult r = modules().delivery_module.createNode(cfg.dump());
        // Two paths can race to bring the node up (the UI's button and the
        // autobroadcast hook). The second one is told the context already
        // exists, which is success by another name.
        const bool alreadyUp = !r.success &&
            (r.error.find("already") != std::string::npos ||
             r.error.find("Already") != std::string::npos);
        if (!r.success && !alreadyUp) {
            std::lock_guard<std::mutex> lk(m_mu);
            m_netStatus = 3;
            m_netError  = r.error.empty() ? "createNode failed" : r.error;
            return {false, {}, m_netError};
        }
        m_nodeCreated = true;
    }

    wireDeliveryEvents();

    const StdLogosResult s = modules().delivery_module.start();
    if (!s.success) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_netStatus = 3;
        m_netError  = s.error.empty() ? "start failed" : s.error;
        return {false, {}, m_netError};
    }

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_started = true;
        if (m_netStatus < 2) m_netStatus = 2;
    }

    // Everyone listens to the directory: it is how a viewer sees who is live
    // without asking a server.
    modules().delivery_module.subscribe(kDirectoryTopic);
    return {true, "started"};
}

// ── broadcasting ─────────────────────────────────────────────────────────

StdLogosResult BeaconCoreImpl::startBroadcast(const std::string& title)
{
    if (!m_station.valid())
        return {false, {}, "no station key"};

    const StdLogosResult net = startNetwork();
    if (!net.success) return net;

    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_broadcasting) return {true, m_ingest->url()};
        m_title = title.empty() ? ("Station " + m_station.shortId()) : title;
        m_seq   = 0;
        m_published = m_publishedBytes = 0;
        m_pendingFragment.clear();
    }

    std::string err;
    if (!m_ingest->start([this](const uint8_t* d, size_t n) { onIngest(d, n); }, err)) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_lastError = err;
        return {false, {}, err};
    }

    // A broadcaster watches their own stream straight from the buffer, so the
    // preview costs no network round trip.
    m_buffer->reset();
    m_buffer->reopen();

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_broadcasting = true;
        m_watching.clear();
        m_watchTopic.clear();
    }
    return {true, m_ingest->url()};
}

StdLogosResult BeaconCoreImpl::stopBroadcast()
{
    m_ingest->stop();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_broadcasting) return {true, "not broadcasting"};
        m_broadcasting = false;
        m_pendingFragment.clear();
    }
    m_buffer->reset();
    return {true, "stopped"};
}

void BeaconCoreImpl::onIngest(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_broadcasting) return;
    m_pendingFragment.insert(m_pendingFragment.end(), data, data + len);
    while (m_pendingFragment.size() >= kFragmentBytes)
        flushLocked();
}

void BeaconCoreImpl::flushLocked()
{
    if (m_pendingFragment.empty()) return;

    const size_t take = std::min(m_pendingFragment.size(), kFragmentBytes);
    std::vector<uint8_t> payload(m_pendingFragment.begin(), m_pendingFragment.begin() + take);
    m_pendingFragment.erase(m_pendingFragment.begin(), m_pendingFragment.begin() + take);

    const uint64_t seq    = m_seq++;
    const int64_t  sentMs = nowMs();
    const auto     signed_ = beacon::wire::signedRegion(seq, sentMs, payload.data(), payload.size());
    const auto     sig     = m_station.sign(signed_);
    if (sig.size() != beacon::wire::kSigLen) {
        m_lastError = "signing failed";
        return;
    }

    const auto packet = beacon::wire::encode(m_station.publicKey(), seq, sentMs,
                                             sig.data(), payload.data(), payload.size());
    const std::string topic = mediaTopic(m_station.publicKeyHex());

    m_published++;
    m_publishedBytes += payload.size();
    m_lastFlushMs = sentMs;

    // Local preview straight from the buffer, and the wire via the pump —
    // sending here would hold m_mu across an IPC round trip.
    m_buffer->push(seq, payload);
    {
        std::lock_guard<std::mutex> out(m_outMu);
        m_outbox.emplace_back(topic, packet);
    }
}

// ── watching ─────────────────────────────────────────────────────────────

StdLogosResult BeaconCoreImpl::watch(const std::string& stationHex)
{
    uint8_t key[32];
    if (!beacon::Station::hexToKey(stationHex, key))
        return {false, {}, "a station key is 64 hex characters"};

    const StdLogosResult net = startNetwork();
    if (!net.success) return net;

    stopWatching();
    if (m_broadcasting) stopBroadcast();

    const std::string topic = mediaTopic(stationHex);
    const StdLogosResult sub = modules().delivery_module.subscribe(topic);
    if (!sub.success) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_lastError = sub.error.empty() ? "subscribe failed" : sub.error;
        return {false, {}, m_lastError};
    }

    m_buffer->reset();
    m_buffer->reopen();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        std::memcpy(m_watchKey, key, sizeof(key));
        m_watching   = stationHex;
        m_watchTopic = topic;
        m_rejected   = 0;
    }
    return {true, topic};
}

StdLogosResult BeaconCoreImpl::stopWatching()
{
    std::string topic;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_watching.empty()) return {true, "not watching"};
        topic = m_watchTopic;
        m_watching.clear();
        m_watchTopic.clear();
    }
    modules().delivery_module.unsubscribe(topic);
    m_buffer->reset();
    return {true, "stopped"};
}

void BeaconCoreImpl::onMessage(const std::string& topic, const std::vector<uint8_t>& payload)
{
    if (topic == kDirectoryTopic) {
        // Announcements are written by strangers: treat them as data, and never
        // as a reason to do anything but list a station the user may choose.
        try {
            const auto j = json::parse(payload.begin(), payload.end());
            const std::string station = j.value("station", "");
            if (station.size() != 64) return;
            std::string title = j.value("title", "");
            if (title.size() > 120) title = title.substr(0, 120);

            std::lock_guard<std::mutex> lk(m_mu);
            auto& listing = m_directory[station];
            listing.title      = title;
            listing.lastSeenMs = nowMs();
        } catch (...) {
        }
        return;
    }

    std::string watching;
    uint8_t key[32];
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_watching.empty() || topic != m_watchTopic) return;
        watching = m_watching;
        std::memcpy(key, m_watchKey, sizeof(key));
    }

    beacon::wire::Packet pkt;
    if (!beacon::wire::decode(payload, pkt)) return;

    // The station's key is its identity: anything else publishing here is noise,
    // however well-formed.
    if (std::memcmp(pkt.pubkey, key, sizeof(key)) != 0) {
        std::lock_guard<std::mutex> lk(m_mu);
        ++m_rejected;
        return;
    }
    const auto signed_ = beacon::wire::signedRegion(pkt.seq, pkt.sentMs,
                                                    pkt.payload.data(), pkt.payload.size());
    if (!beacon::Station::verify(key, pkt.sig, signed_)) {
        std::lock_guard<std::mutex> lk(m_mu);
        ++m_rejected;
        return;
    }

    m_buffer->push(pkt.seq, pkt.payload);
}

// ── pump ─────────────────────────────────────────────────────────────────

void BeaconCoreImpl::pumpLoop()
{
    while (m_pumping) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const int64_t now = nowMs();

        std::string announce;
        {
            std::lock_guard<std::mutex> lk(m_mu);

            // Ship a partial fragment rather than hold video hostage to a
            // buffer that is filling slowly.
            if (m_broadcasting && !m_pendingFragment.empty() && now - m_lastFlushMs >= kFlushMs)
                flushLocked();

            if (m_broadcasting && now - m_lastAnnounceMs >= kAnnounceMs) {
                m_lastAnnounceMs = now;
                json a;
                a["station"] = m_station.publicKeyHex();
                a["title"]   = m_title;
                a["ts"]      = now;
                announce = a.dump();
            }

            for (auto it = m_directory.begin(); it != m_directory.end();) {
                if (now - it->second.lastSeenMs > kListingTtlMs) it = m_directory.erase(it);
                else ++it;
            }
        }

        // Everything that goes on the wire is sent from here, with no lock held.
        std::deque<std::pair<std::string, std::vector<uint8_t>>> outbox;
        {
            std::lock_guard<std::mutex> out(m_outMu);
            outbox.swap(m_outbox);
        }
        for (const auto& msg : outbox)
            modules().delivery_module.send(msg.first, msg.second);

        if (!announce.empty()) {
            const std::vector<uint8_t> bytes(announce.begin(), announce.end());
            modules().delivery_module.send(kDirectoryTopic, bytes);
        }
    }
}

// ── state ────────────────────────────────────────────────────────────────

std::string BeaconCoreImpl::state()
{
    json st;
    const int64_t now = nowMs();

    {
        std::lock_guard<std::mutex> lk(m_mu);
        st["station"]     = m_station.publicKeyHex();
        st["shortId"]     = m_station.shortId();
        st["netStatus"]   = m_netStatus;
        st["netError"]    = m_netError;
        st["error"]       = m_lastError;
        st["broadcasting"] = m_broadcasting;
        st["title"]       = m_title;
        st["watching"]    = m_watching;
        st["published"]   = m_published;
        st["publishedBytes"] = m_publishedBytes;
        st["rejected"]    = m_rejected;

        json dir = json::array();
        for (const auto& kv : m_directory) {
            json e;
            e["station"] = kv.first;
            e["title"]   = kv.second.title;
            e["ageMs"]   = now - kv.second.lastSeenMs;
            e["self"]    = (kv.first == m_station.publicKeyHex());
            dir.push_back(e);
        }
        st["directory"] = dir;
    }

    json ing;
    ing["running"]      = m_ingest->running();
    ing["url"]          = m_ingest->url();
    ing["bytesIn"]      = m_ingest->bytesIn();
    ing["lastPacketMs"] = m_ingest->lastPacketMs();
    ing["liveMs"]       = m_ingest->lastPacketMs() ? (now - m_ingest->lastPacketMs()) : -1;
    st["ingest"] = ing;

    json play;
    play["url"]       = m_http->playerUrl();
    play["serving"]   = m_http->running();
    play["fragments"] = m_buffer->fragments();
    play["gaps"]      = m_buffer->gaps();
    play["bytes"]     = m_buffer->bytes();
    st["player"] = play;

    return st.dump();
}
