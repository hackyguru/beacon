#ifndef BEACON_STATION_H
#define BEACON_STATION_H

#include <cstdint>
#include <string>
#include <vector>

namespace beacon {

/**
 * A station's ed25519 identity.
 *
 * The public key *is* the station address: viewers ask for a key, not a name,
 * so a station cannot be impersonated by someone who merely knows where it
 * publishes. The secret key never leaves the module's own data directory.
 */
class Station
{
public:
    /** Loads the keypair from `dir/station.key`, creating one if absent. */
    bool loadOrCreate(const std::string& dir, std::string& error);

    /** Uses a key held only in memory — for viewers, which never sign. */
    void createEphemeral();

    bool valid() const { return m_haveKeys; }

    const uint8_t* publicKey() const { return m_pk; }
    std::string    publicKeyHex() const;
    /** First 8 hex characters — enough to recognise a station in a list. */
    std::string    shortId() const;

    /** Detached signature over `data`. Empty if this station cannot sign. */
    std::vector<uint8_t> sign(const std::vector<uint8_t>& data) const;

    static bool verify(const uint8_t* pubkey, const std::vector<uint8_t>& sig,
                       const std::vector<uint8_t>& data);

    /**
     * The whole keypair as hex, for backing a station up or moving it to
     * another machine. This IS the station: anyone holding it can broadcast as
     * you, and there is no revocation.
     */
    std::string exportSecretHex() const;

    /** Replaces the stored keypair with an exported one. */
    bool importSecretHex(const std::string& dir, const std::string& hex, std::string& error);

    /** 64 hex characters to raw bytes; false if malformed. */
    static bool hexToKey(const std::string& hex, uint8_t out[32]);
    static std::string toHex(const uint8_t* bytes, size_t len);

private:
    uint8_t m_pk[32]{};
    uint8_t m_sk[64]{};
    bool    m_haveKeys = false;
    bool    m_canSign  = false;
};

} // namespace beacon

#endif // BEACON_STATION_H
