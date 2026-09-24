<p align="center">
	<img width="150" height="150" src="ui/icons/beacon.png" alt="Beacon logo">
</p>

<h1 align="center">Beacon</h1>

<p align="center">
	Live streaming with no server in between. Stream from OBS, and anyone with your station key can watch — relayed peer to peer, with nothing in the middle to take down.
</p>

<p align="center">
	<a href="#why-beacon">Why Beacon</a>
	·
	<a href="#how-a-station-works">How a station works</a>
	·
	<a href="#get-started">Get started</a>
	·
	<a href="#local-development">Develop</a>
	·
	<a href="#status">Status</a>
</p>

<p align="center">
	<img src="https://img.shields.io/badge/runs%20in-Logos%20Basecamp-ED7B58" alt="Runs in Logos Basecamp">
	<img src="https://img.shields.io/badge/transport-Logos%20Delivery-lightgrey" alt="Logos Delivery">
	<img src="https://img.shields.io/badge/ingest-OBS%20%7C%20ffmpeg%20%7C%20vMix-lightgrey" alt="OBS, ffmpeg, vMix">
	<img src="https://img.shields.io/badge/signed-ed25519-brightgreen" alt="ed25519 signed">
</p>

> [!WARNING]
> **Beacon is alpha software, provided as is and without warranty of any kind.**
> A station's topic is public: anyone who knows the key can watch, and the
> stream is **not encrypted**. Viewers are visible to the relay mesh they
> subscribe through. Do not broadcast anything you would not put on a public
> web page.

## Why Beacon

- **There is no origin server.** Not ours, not a CDN's. Every viewer relays to other viewers, so a thousand people watching costs the broadcaster roughly what one does.
- **A station is a keypair, not a name.** Every fragment is signed, and viewers drop anything that does not verify. Knowing where a station publishes is not enough to publish there — nobody can hijack your stream.
- **Bring your own encoder.** OBS, ffmpeg, vMix, Ecamm, a phone encoder: anything that can send MPEG-TS to a local port. Beacon never re-encodes, so quality and CPU cost are whatever you set in your own software.
- **No accounts, no keys to register, no signup.** Go live, share your key.
- **Sub-two-second latency,** rather than the ten seconds HLS spends on segments.

## How a station works

A station's public key is its address. The broadcaster publishes to `/beacon/1/<station>/ts`, and a viewer subscribes to the same topic and verifies every fragment against that key.

| | |
| --- | --- |
| **Ingest** | Your software pushes MPEG-TS at a loopback UDP port. Local only: a cloud studio cannot reach it, and neither can anyone else |
| **Fragments** | The stream is cut into 8 KiB slices — 44 transport packets — each signed with ed25519 and published. About 13 a second at 700 kbit/s |
| **Signature** | Over the sequence number, the timestamp and the payload. A fragment from any other key is dropped and counted |
| **Reordering** | Gossipsub delivers out of order, so fragments are held briefly in sequence. Once the window fills, the gap is conceded and playback continues |
| **Playback** | The viewer serves the reassembled stream on loopback HTTP, and the WebView Basecamp bundles decodes and renders it with mpegts.js |
| **Discovery** | A live station announces itself every three seconds on a shared directory topic. No registry, no server |

**Why MPEG-TS and not RTMP or MP4.** A transport stream can be cut anywhere, repeats its own headers, and survives losing a piece — a viewer arriving mid-broadcast starts at the next keyframe with nothing negotiated. That is what lets Beacon be a pure transport: no demuxer, no muxer, no codec, and a lost fragment costs a glitch rather than the rest of the stream.

## Get started

**You need**

- [Logos Basecamp](https://github.com/logos-co/logos-basecamp) 0.2 or later.
- `delivery_module` installed in Basecamp. Basecamp 0.2 does not bundle it.
- [Nix](https://nixos.org) with flakes enabled, to build the modules.
- OBS, or anything that speaks MPEG-TS over UDP.

**Build and install**

```bash
cd core && nix build '.#lgx-portable' --out-link result-portable
cd ../ui && nix build '.#lgx-portable' --out-link result-portable
cd .. && ./install.sh
```

**To broadcast**

1. Open Beacon, go to **Broadcast**, give the stream a title and press **Start broadcasting**.
2. Copy the ingest address it shows, for example `udp://127.0.0.1:51631`.
3. In OBS: **Settings ▸ Output ▸ Recording**, type **Custom Output (FFmpeg)**, container **mpegts**, and that address as the URL. Then **Start Recording**.
4. Share your station key. It is on the Broadcast tab.

With plain ffmpeg instead of OBS:

```bash
ffmpeg -re -f avfoundation -i "1:0" \
  -c:v libx264 -preset veryfast -tune zerolatency -g 60 -b:v 700k \
  -c:a aac -b:a 96k -f mpegts "udp://127.0.0.1:<port>?pkt_size=1316"
```

**To watch:** open Beacon, pick a station from **Live now**, or paste a station key.

## Local development

| Command | What it does |
| --- | --- |
| `nix build '.#lgx-portable'` in `core/` | Build the core module |
| `nix build --override-input beacon_core path:../core '.#lgx-portable'` in `ui/` | Build the UI against your local core |
| `./install.sh` | Install both into Basecamp |
| `curl 127.0.0.1:<port>/stats` | The core's own state as JSON, without the UI |

**Environment**

| Variable | What it does |
| --- | --- |
| `BEACON_TCPPORT` | Run as a second instance on this port, with its own station key |
| `BEACON_AUTOBROADCAST` | Go live with this title a few seconds after loading |
| `BEACON_AUTOWATCH` | Watch this station key on load |

**Two instances on one machine.** Two Basecamps collide on the delivery port and share a user directory, so the second one needs its own port and identity:

```sh
open -n /Applications/LogosBasecamp.app
BEACON_TCPPORT=60001 open -n /Applications/LogosBasecamp.app
```

**The network.** The logos.dev fleet moved to cluster 3 while `delivery_module` 0.2.0's preset still says 2, so the core sets `clusterId` explicitly. Drop that once the module ships the new preset.

## Repository map

| Path | What it is |
| --- | --- |
| [`core/`](core/) | The `beacon_core` module, C++ |
| [`core/src/beacon_core_impl.cpp`](core/src/beacon_core_impl.cpp) | Broadcasting, watching, the directory |
| [`core/src/ingest.cpp`](core/src/ingest.cpp) | The loopback port OBS pushes at |
| [`core/src/station.cpp`](core/src/station.cpp) | ed25519 station identity |
| [`core/src/stream_buffer.cpp`](core/src/stream_buffer.cpp) | Reordering and gap handling |
| [`core/src/http_server.cpp`](core/src/http_server.cpp) | The player page and the live stream |
| [`ui/`](ui/) | The `beacon` frontend: Watch and Broadcast |

## Status

Version 0.1.0. Measured on one machine, two Basecamp instances, ffmpeg standing in for OBS:

| | |
| --- | --- |
| **Broadcast** | 480p30 at ~900 kbit/s ingested and published as ~13 signed fragments a second |
| **Watch** | A second instance received 975 of 991 fragments with **zero gaps** and **zero rejected**, holding a steady ~1.2 s behind |
| **Discovery** | The viewer found the station through the directory rather than being told |
| **Across the internet** | Not yet measured. Everything so far is two instances on one machine |
| **Linux** | Not built or tested |

The honest gaps:

- **No encryption.** The topic is public and so is the stream.
- **Viewers are not anonymous** to the mesh they subscribe through.
- **Bandwidth is the real ceiling.** Gossipsub fan-out means viewers upload as well as download; expect 360p–480p to work and 1080p60 not to.
- **One viewer per instance.** The reassembled stream has a single consumer, so the newest player wins.
- **No native capture yet.** Beacon is a transport; your own software does the capturing and encoding.

## Credits

Beacon bundles [mpegts.js](https://github.com/xqq/mpegts.js) (Apache-2.0) so a viewer can render a stream without fetching anything from a server. See [NOTICE](NOTICE).

## Licence

MIT and Apache-2.0. Pick whichever works for you.
