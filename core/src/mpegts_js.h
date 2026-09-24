#ifndef BEACON_MPEGTS_JS_H
#define BEACON_MPEGTS_JS_H

/**
 * mpegts.js, embedded.
 *
 * The viewer has no decoder of its own: it hands a live transport stream to
 * the WebView Basecamp bundles, and mpegts.js turns that into something a
 * <video> element can play. Embedded rather than fetched from a CDN — an app
 * whose point is that it needs no servers should not need one to render.
 *
 * mpegts.js is Apache-2.0 licensed. See NOTICE.
 */
extern const char* const kMpegtsJs;

#endif // BEACON_MPEGTS_JS_H
