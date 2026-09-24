#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "logos_module_context.h"
#include "logos_result.h"

#include "http_server.h"
#include "ingest.h"
#include "station.h"
#include "stream_buffer.h"

/**
 * @brief One-way live streaming over a gossipsub topic.
 *
 * A station is a keypair, not a name. The broadcaster signs every fragment and
 * viewers drop anything that does not verify against the key they asked for, so
 * knowing the topic is not enough to be heard on it. That is the one mechanic a
 * broadcast needs and a call does not: in a call everyone publishes, here
 * exactly one identity may.
 *
 * Nothing here encodes or decodes video. The streamer's own software (OBS,
 * ffmpeg, vMix…) pushes MPEG-TS at a loopback port; Beacon fragments, signs and
 * publishes those bytes untouched. Viewers verify, reorder and hand the stream
 * to the WebView, which decodes it. Beacon is a transport with an ingest port
 * at one end and a player at the other.
 *
 * MPEG-TS rather than RTMP or MP4 because it survives being cut anywhere and
 * repeats its own headers, so a viewer who arrives mid-broadcast starts playing
 * at the next keyframe with nothing negotiated — and a lost fragment costs a
 * glitch rather than the rest of the stream.
 */
class BeaconCoreImpl : public LogosModuleContext
{
public:
    BeaconCoreImpl();
    ~BeaconCoreImpl();

    /// Bring the delivery node up and subscribe to the station directory.
    StdLogosResult startNetwork();

    /**
     * Start broadcasting: opens the ingest port.
     * `listed` false keeps the station out of the public directory, so the only
     * way to find it is a key you handed out yourself.
     */
    StdLogosResult startBroadcast(const std::string& title, bool listed);
    StdLogosResult stopBroadcast();

    /// Watch a station by its 64-character public key.
    StdLogosResult watch(const std::string& stationHex);
    StdLogosResult stopWatching();

    /// JSON snapshot for the UI.
    std::string state();

    /// This machine's own station key, in hex.
    std::string stationKey();

    /// The whole keypair, for backup. Treat it as the station itself.
    std::string exportStation();

    /// Replace this machine's station with a backed-up one.
    StdLogosResult importStation(const std::string& secretHex);

protected:
    void onContextReady() override;

private:
    struct Listing {
        std::string title;
        int64_t     lastSeenMs = 0;
    };

    void wireDeliveryEvents();
    void onMessage(const std::string& topic, const std::vector<uint8_t>& payload);
    void onIngest(const uint8_t* data, size_t len);
    void flushLocked();                 // publish the pending fragment
    void pumpLoop();                    // flush on a timer, announce, prune
    static std::string mediaTopic(const std::string& stationHex);
    void reserveSequence(uint64_t upTo);   // persist the sequence high-water mark
    std::string keyDir() const;

    std::unique_ptr<beacon::HttpServer>   m_http;
    std::unique_ptr<beacon::StreamBuffer> m_buffer;
    std::unique_ptr<beacon::Ingest>       m_ingest;
    beacon::Station                       m_station;

    std::thread       m_pump;
    std::atomic<bool> m_pumping{false};

    mutable std::mutex m_mu;
    bool        m_nodeCreated  = false;
    bool        m_started      = false;
    bool        m_eventsWired  = false;
    int         m_netStatus    = 0;      // 0 off · 1 connecting · 2 connected · 3 error
    std::string m_netError;
    std::string m_lastError;
    std::string m_keyDir;

    bool        m_broadcasting = false;
    bool        m_listed       = true;
    std::string m_title;
    // Never restarts, and survives a restart of the app. A sequence that began
    // again at zero let an attacker replay fragments from a longer past
    // broadcast whose numbers ran ahead of the live ones.
    uint64_t    m_seq          = 0;
    uint64_t    m_seqReserved  = 0;
    std::vector<uint8_t> m_pendingFragment;
    int64_t     m_lastFlushMs  = 0;
    int64_t     m_lastAnnounceMs = 0;
    uint64_t    m_published    = 0;
    uint64_t    m_publishedBytes = 0;

    std::string m_watching;              // station hex, empty when not watching
    uint8_t     m_watchKey[32]{};
    std::string m_watchTopic;
    uint64_t    m_rejected     = 0;      // fragments that failed verification
    uint64_t    m_stale        = 0;      // fragments too old to be this broadcast

    std::map<std::string, Listing> m_directory;

    // Publishing goes through the pump thread. delivery_module::send is a
    // synchronous IPC call, and making it while holding m_mu wedged every
    // reader of the state (the UI polls it once a second, /stats likewise).
    std::mutex m_outMu;
    std::deque<std::pair<std::string, std::vector<uint8_t>>> m_outbox;
};
