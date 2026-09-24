// Beacon — live streaming with no server in between.
//
// Two halves. Watch: pick a live station (or paste its key) and the core
// verifies, reorders and serves the stream to the WebView below. Broadcast:
// the core opens a loopback ingest port, and whatever OBS pushes at it is
// signed and published untouched.
//
// The player is a WebView on purpose. A QML plugin has no video sink —
// Basecamp ships no Qt Multimedia — so the stream goes over loopback HTTP and
// the system's web engine decodes, renders and keeps it in sync.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtWebView
import QtCore

import Logos.Theme
import Logos.Controls

Rectangle {
    id: root
    width: 1000
    height: 760
    color: Theme.palette.background

    property var st: ({})
    property int tab: 0                                   // 0 watch · 1 broadcast
    property string lastError: ""
    property string copied: ""

    readonly property string station: st.station || ""
    readonly property bool broadcasting: st.broadcasting === true
    readonly property string watching: st.watching || ""
    readonly property bool playing: broadcasting || watching.length > 0
    readonly property var ingest: st.ingest || ({})
    readonly property var player: st.player || ({})
    readonly property var directory: st.directory || []
    readonly property int netStatus: st.netStatus || 0

    Settings {
        id: prefs
        category: "beacon"
        property string title: ""
        property string lastStation: ""
    }

    // ── bridge ───────────────────────────────────────────────────────
    function call(method, args, cb) {
        if (typeof logos === "undefined" || !logos.callModuleAsync) {
            lastError = "The Logos bridge is unavailable.";
            return;
        }
        logos.callModuleAsync("beacon_core", method, args, function (raw) {
            var r = unwrap(raw, null);
            if (r && typeof r === "object" && r.success === false)
                lastError = r.error || "Something went wrong.";
            if (cb)
                cb(r);
            refresh();
        });
    }
    function unwrap(raw, def) {
        if (raw === null || raw === undefined)
            return def;
        var v = raw;
        for (var i = 0; i < 3 && typeof v === "string"; ++i) {
            try {
                v = JSON.parse(v);
            } catch (e) {
                return (i === 0) ? def : v;
            }
        }
        return v;
    }
    function refresh() {
        call2("state", function (s) {
            if (s && typeof s === "object")
                root.st = s;
        });
    }
    function call2(method, cb) {
        if (typeof logos === "undefined" || !logos.callModuleAsync)
            return;
        logos.callModuleAsync("beacon_core", method, [], function (raw) {
            cb(unwrap(raw, null));
        });
    }

    // ── actions ──────────────────────────────────────────────────────
    function startBroadcast() {
        lastError = "";
        prefs.title = titleField.text;
        call("startBroadcast", [titleField.text]);
    }
    function stopBroadcast() {
        call("stopBroadcast", []);
    }
    function watch(key) {
        lastError = "";
        prefs.lastStation = key;
        root.tab = 0;
        call("watch", [key]);
    }
    function stopWatching() {
        call("stopWatching", []);
    }

    // ── helpers ──────────────────────────────────────────────────────
    function shortKey(k) {
        return k && k.length > 16 ? k.substring(0, 8) + "…" + k.substring(k.length - 4) : (k || "");
    }
    function kbps(bytes, ms) {
        return ms > 0 ? Math.round(bytes * 8 / ms) : 0;
    }
    function human(bytes) {
        if (!bytes)
            return "0 B";
        if (bytes > 1e9)
            return (bytes / 1e9).toFixed(1) + " GB";
        if (bytes > 1e6)
            return (bytes / 1e6).toFixed(1) + " MB";
        if (bytes > 1e3)
            return (bytes / 1e3).toFixed(0) + " KB";
        return bytes + " B";
    }
    function netLabel(s) {
        return s === 2 ? "Online" : s === 1 ? "Connecting" : s === 3 ? "Network error" : "Offline";
    }
    function netColor(s) {
        return s === 2 ? Theme.palette.success : s === 1 ? Theme.palette.warning : s === 3 ? Theme.palette.error : Theme.palette.textTertiary;
    }

    TextEdit {
        id: clip
        visible: false
    }
    function copy(t, key) {
        clip.text = t;
        clip.selectAll();
        clip.copy();
        copied = key;
        copyTimer.restart();
    }
    Timer {
        id: copyTimer
        interval: 1600
        onTriggered: root.copied = ""
    }
    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: root.refresh()
    }
    Component.onCompleted: {
        titleField.text = prefs.title;
        keyField.text = prefs.lastStation;
        refresh();
    }

    Gradient {
        id: accentGrad
        GradientStop {
            position: 0.0
            color: "#F28E6B"
        }
        GradientStop {
            position: 1.0
            color: "#E1613A"
        }
    }

    // ── building blocks (shared idiom with Chorus and Persona) ───────
    component Card: Rectangle {
        id: cardRoot
        default property alias content: cardCol.data
        property string title: ""
        property int pad: Theme.spacing.large
        Layout.fillWidth: true
        color: Theme.palette.backgroundTertiary
        border.color: Theme.palette.borderSubtle
        border.width: 1
        radius: Theme.spacing.radiusLarge
        implicitHeight: cardCol.implicitHeight + pad * 2
        ColumnLayout {
            id: cardCol
            anchors {
                fill: parent
                margins: cardRoot.pad
            }
            spacing: Theme.spacing.medium
            RowLayout {
                visible: cardRoot.title.length > 0
                spacing: Theme.spacing.small
                Rectangle {
                    implicitWidth: 4
                    implicitHeight: 12
                    radius: 2
                    color: Theme.palette.primary
                }
                LogosText {
                    text: cardRoot.title
                    color: Theme.palette.textSecondary
                    font.pixelSize: 11
                    font.weight: Theme.typography.weightMedium
                    font.letterSpacing: 0.8
                    font.capitalization: Font.AllUppercase
                }
            }
        }
    }

    component ActionButton: Control {
        id: btn
        property string text: ""
        property bool accent: false
        property bool danger: false
        signal clicked
        hoverEnabled: true
        implicitHeight: 40
        implicitWidth: btnLabel.implicitWidth + 36
        readonly property bool isActive: btnMa.pressed || btn.hovered
        scale: (btnMa.pressed && btn.enabled) ? 0.96 : 1.0
        Behavior on scale {
            NumberAnimation {
                duration: 90
            }
        }
        background: Rectangle {
            radius: Theme.spacing.radiusXlarge
            gradient: (btn.accent && btn.enabled) ? accentGrad : null
            color: !btn.enabled ? Theme.palette.backgroundMuted : btn.danger ? Theme.colors.getColor(Theme.palette.error, btn.isActive ? 0.85 : 0.72) : (btn.isActive ? Theme.palette.backgroundMuted : Theme.palette.backgroundSecondary)
            border.width: (btn.accent || btn.danger) && btn.enabled ? 0 : 1
            border.color: !btn.enabled ? Theme.palette.border : (btn.isActive ? Theme.palette.overlayOrange : Theme.palette.border)
            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }
        }
        contentItem: LogosText {
            id: btnLabel
            text: btn.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: Theme.typography.secondaryText
            font.weight: (btn.accent || btn.danger) ? Theme.typography.weightBold : Theme.typography.weightMedium
            color: !btn.enabled ? Theme.palette.textMuted : btn.accent ? "#241511" : btn.danger ? "#FFFFFF" : Theme.palette.text
        }
        MouseArea {
            id: btnMa
            anchors.fill: parent
            enabled: btn.enabled
            cursorShape: btn.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: btn.clicked()
        }
    }

    component MiniButton: Control {
        id: mb
        property string label: ""
        signal clicked
        hoverEnabled: true
        implicitHeight: 26
        implicitWidth: mbLabel.implicitWidth + 20
        background: Rectangle {
            radius: Theme.spacing.radiusPill
            color: mb.hovered ? Theme.palette.backgroundMuted : "transparent"
            border.width: 1
            border.color: mb.hovered ? Theme.palette.overlayOrange : Theme.palette.borderSubtle
        }
        contentItem: LogosText {
            id: mbLabel
            text: mb.label
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: 11
            font.weight: Theme.typography.weightMedium
            color: mb.hovered ? Theme.palette.text : Theme.palette.textSecondary
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: mb.clicked()
        }
    }

    component FieldLabel: LogosText {
        color: Theme.palette.textSecondary
        font.pixelSize: 11
        font.weight: Theme.typography.weightMedium
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }

    component Mono: LogosText {
        font.family: "Menlo"
        font.pixelSize: 11
        color: Theme.palette.textSecondary
    }

    component LiveDot: Row {
        property bool live: false
        spacing: 6
        Rectangle {
            width: 8
            height: 8
            radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: parent.live ? Theme.palette.error : Theme.palette.textTertiary
            SequentialAnimation on opacity {
                running: parent.parent.live
                loops: Animation.Infinite
                NumberAnimation {
                    from: 1
                    to: 0.3
                    duration: 700
                }
                NumberAnimation {
                    from: 0.3
                    to: 1
                    duration: 700
                }
            }
        }
    }

    // ── layout ───────────────────────────────────────────────────────
    ColumnLayout {
        anchors {
            fill: parent
            margins: Theme.spacing.xlarge
        }
        spacing: Theme.spacing.large

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.medium
            Image {
                source: "icons/beacon.png"
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                sourceSize: Qt.size(128, 128)
                smooth: true
            }
            LogosText {
                text: "Beacon"
                color: Theme.palette.text
                font.pixelSize: Theme.typography.subtitleText
                font.weight: Theme.typography.weightBold
                Layout.rightMargin: Theme.spacing.small
            }
            LogosTabBar {
                id: tabBar
                Layout.fillWidth: true
                currentIndex: root.tab
                onCurrentIndexChanged: root.tab = currentIndex
                LogosTabButton {
                    text: "Watch"
                }
                LogosTabButton {
                    text: "Broadcast"
                }
            }
            Rectangle {
                radius: Theme.spacing.radiusPill
                color: Theme.palette.backgroundInset
                border.width: 1
                border.color: Theme.palette.borderHairline
                implicitHeight: 30
                implicitWidth: netRow.implicitWidth + 24
                RowLayout {
                    id: netRow
                    anchors.centerIn: parent
                    spacing: 6
                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: root.netColor(root.netStatus)
                    }
                    LogosText {
                        text: root.netLabel(root.netStatus)
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.text
                    }
                }
            }
        }

        Rectangle {
            id: errBanner
            Layout.fillWidth: true
            readonly property string msg: root.lastError.length ? root.lastError : (root.st.netError || "")
            visible: msg.length > 0
            color: Theme.colors.getColor(Theme.palette.error, 0.10)
            border.color: Theme.colors.getColor(Theme.palette.error, 0.45)
            border.width: 1
            radius: Theme.spacing.radiusMedium
            implicitHeight: errRow.implicitHeight + Theme.spacing.medium * 2
            RowLayout {
                id: errRow
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: Theme.spacing.medium
                }
                spacing: Theme.spacing.small
                LogosText {
                    Layout.fillWidth: true
                    text: errBanner.msg
                    wrapMode: Text.Wrap
                    color: Theme.palette.error
                    font.pixelSize: Theme.typography.secondaryText
                }
                MiniButton {
                    label: "Dismiss"
                    onClicked: root.lastError = ""
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing.large

            // ── the player ───────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 3
                spacing: Theme.spacing.medium

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Theme.spacing.radiusLarge
                    color: "#000000"
                    border.width: 1
                    border.color: root.playing ? Theme.colors.getColor(Theme.palette.primary, 0.5) : Theme.palette.borderSubtle
                    clip: true

                    WebView {
                        id: view
                        anchors.fill: parent
                        anchors.margins: 1
                        visible: root.playing && (root.player.url || "").length > 0
                        url: visible ? root.player.url : ""
                    }

                    ColumnLayout {
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 60, 320)
                        visible: !view.visible
                        spacing: Theme.spacing.small
                        LogosText {
                            Layout.alignment: Qt.AlignHCenter
                            text: root.tab === 1 ? "Not broadcasting" : "Nothing playing"
                            color: Theme.palette.text
                            font.pixelSize: Theme.typography.primaryText
                            font.weight: Theme.typography.weightMedium
                        }
                        LogosText {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: root.tab === 1 ? "Start a broadcast, then point OBS at the ingest address."
                                                 : "Pick a live station, or paste a station key."
                            wrapMode: Text.Wrap
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.secondaryText
                        }
                    }

                    // What's on: station and title, over the video.
                    Rectangle {
                        visible: root.playing
                        anchors {
                            left: parent.left
                            top: parent.top
                            margins: Theme.spacing.medium
                        }
                        radius: Theme.spacing.radiusPill
                        color: Theme.colors.getColor("#000000", 0.55)
                        border.width: 1
                        border.color: Theme.palette.borderHairline
                        implicitHeight: 28
                        implicitWidth: liveRow.implicitWidth + 22
                        RowLayout {
                            id: liveRow
                            anchors.centerIn: parent
                            spacing: 8
                            LiveDot {
                                live: root.playing
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            LogosText {
                                text: root.broadcasting ? "LIVE · you" : "LIVE · " + root.shortKey(root.watching)
                                color: "#FFFFFF"
                                font.pixelSize: 11
                                font.weight: Theme.typography.weightBold
                            }
                        }
                    }
                }

                // Stream health
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.medium
                    visible: root.playing
                    Mono {
                        text: root.broadcasting
                            ? "published " + (root.st.published || 0) + " fragments · " + root.human(root.st.publishedBytes || 0)
                            : "received " + (root.player.fragments || 0) + " fragments · " + root.human(root.player.bytes || 0)
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Mono {
                        visible: !root.broadcasting
                        text: "gaps " + (root.player.gaps || 0)
                        color: (root.player.gaps || 0) > 0 ? Theme.palette.warning : Theme.palette.textTertiary
                    }
                    Mono {
                        visible: !root.broadcasting && (root.st.rejected || 0) > 0
                        text: "rejected " + root.st.rejected
                        color: Theme.palette.error
                    }
                    MiniButton {
                        label: root.broadcasting ? "Stop broadcast" : "Stop watching"
                        onClicked: root.broadcasting ? root.stopBroadcast() : root.stopWatching()
                    }
                }
            }

            // ── the side panel ───────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 2
                Layout.maximumWidth: 400
                spacing: Theme.spacing.large

                // Watch
                Card {
                    visible: root.tab === 0
                    title: "Live now"
                    Layout.alignment: Qt.AlignTop

                    Repeater {
                        model: root.directory
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 54
                            radius: Theme.spacing.radiusMedium
                            color: rowMa.containsMouse ? Theme.palette.backgroundMuted : Theme.palette.backgroundInset
                            border.width: 1
                            border.color: modelData.station === root.watching ? Theme.palette.primary : Theme.palette.borderHairline
                            RowLayout {
                                anchors {
                                    fill: parent
                                    leftMargin: Theme.spacing.medium
                                    rightMargin: Theme.spacing.medium
                                }
                                spacing: Theme.spacing.small
                                LiveDot {
                                    live: true
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    LogosText {
                                        Layout.fillWidth: true
                                        text: (modelData.title && modelData.title.length) ? modelData.title : "Untitled"
                                        elide: Text.ElideRight
                                        color: Theme.palette.text
                                        font.pixelSize: Theme.typography.secondaryText
                                        font.weight: Theme.typography.weightMedium
                                    }
                                    Mono {
                                        text: root.shortKey(modelData.station) + (modelData.self ? " · you" : "")
                                        color: Theme.palette.textTertiary
                                    }
                                }
                                MiniButton {
                                    label: modelData.station === root.watching ? "Watching" : "Watch"
                                    onClicked: root.watch(modelData.station)
                                }
                            }
                            MouseArea {
                                id: rowMa
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.NoButton
                            }
                        }
                    }

                    LogosText {
                        visible: root.directory.length === 0
                        Layout.fillWidth: true
                        text: root.netStatus === 2 ? "No stations are live. A station appears here within a few seconds of going live."
                                                   : "Connecting to the network…"
                        wrapMode: Text.Wrap
                        color: Theme.palette.textTertiary
                        font.pixelSize: Theme.typography.secondaryText
                    }
                }

                Card {
                    visible: root.tab === 0
                    title: "Watch by key"
                    Layout.alignment: Qt.AlignTop
                    LogosTextField {
                        id: keyField
                        Layout.fillWidth: true
                        implicitHeight: 40
                        placeholderText: "64-character station key"
                    }
                    Connections {
                        target: keyField.textInput
                        function onAccepted() {
                            root.watch(keyField.text.trim());
                        }
                    }
                    ActionButton {
                        Layout.fillWidth: true
                        accent: true
                        text: "Watch"
                        enabled: keyField.text.trim().length === 64
                        onClicked: root.watch(keyField.text.trim())
                    }
                    LogosText {
                        Layout.fillWidth: true
                        text: "A station's key is its identity: fragments signed by any other key are dropped, so nobody can take over a stream by publishing on its topic."
                        wrapMode: Text.Wrap
                        color: Theme.palette.textTertiary
                        font.pixelSize: 11
                    }
                }

                // Broadcast
                Card {
                    visible: root.tab === 1
                    title: root.broadcasting ? "You are live" : "Go live"
                    Layout.alignment: Qt.AlignTop

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: !root.broadcasting
                        FieldLabel {
                            text: "Title"
                        }
                        LogosTextField {
                            id: titleField
                            Layout.fillWidth: true
                            implicitHeight: 40
                            placeholderText: "What are you streaming?"
                        }
                    }
                    ActionButton {
                        Layout.fillWidth: true
                        visible: !root.broadcasting
                        accent: true
                        text: "Start broadcasting"
                        onClicked: root.startBroadcast()
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.broadcasting
                        spacing: Theme.spacing.small

                        FieldLabel {
                            text: "Send video here"
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 40
                            radius: Theme.spacing.radiusSmall
                            color: Theme.palette.backgroundInset
                            border.width: 1
                            border.color: Theme.palette.borderHairline
                            RowLayout {
                                anchors {
                                    fill: parent
                                    leftMargin: Theme.spacing.medium
                                    rightMargin: 6
                                }
                                Mono {
                                    Layout.fillWidth: true
                                    text: root.ingest.url || "—"
                                    color: Theme.palette.text
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                                MiniButton {
                                    label: root.copied === "ingest" ? "✓" : "Copy"
                                    onClicked: root.copy(root.ingest.url || "", "ingest")
                                }
                            }
                        }
                        LogosText {
                            Layout.fillWidth: true
                            text: "In OBS: Settings ▸ Output ▸ Recording, type \"Custom Output (FFmpeg)\", container mpegts, and that address as the URL. Then Start Recording."
                            wrapMode: Text.Wrap
                            color: Theme.palette.textTertiary
                            font.pixelSize: 11
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spacing.small
                            implicitHeight: 1
                            color: Theme.palette.borderHairline
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Mono {
                                text: (root.ingest.bytesIn || 0) > 0 ? "receiving · " + root.human(root.ingest.bytesIn) : "waiting for video…"
                                color: (root.ingest.bytesIn || 0) > 0 ? Theme.palette.success : Theme.palette.warning
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            ActionButton {
                                danger: true
                                text: "Stop"
                                onClicked: root.stopBroadcast()
                            }
                        }
                    }
                }

                Card {
                    visible: root.tab === 1
                    title: "Your station key"
                    Layout.alignment: Qt.AlignTop
                    Mono {
                        Layout.fillWidth: true
                        text: root.station.length ? root.station : "—"
                        wrapMode: Text.WrapAnywhere
                        color: Theme.palette.text
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        MiniButton {
                            label: root.copied === "station" ? "✓ Copied" : "Copy key"
                            onClicked: root.copy(root.station, "station")
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                    }
                    LogosText {
                        Layout.fillWidth: true
                        text: "Share this so people can watch you. It stays the same for this machine; the secret half never leaves it."
                        wrapMode: Text.Wrap
                        color: Theme.palette.textTertiary
                        font.pixelSize: 11
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }
        }
    }
}
