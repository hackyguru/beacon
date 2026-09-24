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
    // The WebView's url must NOT be bound to st: st is replaced by every poll,
    // and rebinding reloads the page — which restarted the video once a second
    // and looked like flicker. Only a genuinely different URL is assigned.
    property string playerUrl: ""
    readonly property var directory: st.directory || []
    readonly property int netStatus: st.netStatus || 0

    Settings {
        id: prefs
        category: "beacon"
        property string title: ""
        property string lastStation: ""
        property bool listed: true
    }
    property bool showKeyTools: false

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
            if (!s || typeof s !== "object")
                return;
            root.st = s;
            const wanted = (s.player && s.player.url) ? s.player.url : "";
            const show = (s.broadcasting === true || (s.watching || "").length > 0) && wanted.length > 0;
            const next = show ? wanted : "";
            if (next !== root.playerUrl)
                root.playerUrl = next;
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
        call("startBroadcast", [titleField.text, prefs.listed]);
    }
    function exportStation(cb) {
        call2("exportStation", function (hex) {
            cb(typeof hex === "string" ? hex : "");
        });
    }
    function importStation(hex) {
        lastError = "";
        call("importStation", [hex.trim()], function (r) {
            if (r && r.success !== false)
                importField.text = "";
        });
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

    component Chip: Rectangle {
        id: chip
        property string label: ""
        property color tint: Theme.palette.primary
        radius: Theme.spacing.radiusPill
        color: Theme.colors.getColor(chip.tint, 0.13)
        border.width: 1
        border.color: Theme.colors.getColor(chip.tint, 0.45)
        implicitHeight: 26
        implicitWidth: chipLabel.implicitWidth + 22
        LogosText {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.label
            font.pixelSize: 11
            font.weight: Theme.typography.weightMedium
            color: chip.tint
        }
    }

    component NoticeBanner: Rectangle {
        id: nb
        property color tint: Theme.palette.info
        property string text: ""
        Layout.fillWidth: true
        color: Theme.colors.getColor(nb.tint, 0.10)
        border.color: Theme.colors.getColor(nb.tint, 0.40)
        border.width: 1
        radius: Theme.spacing.radiusMedium
        implicitHeight: nbText.implicitHeight + Theme.spacing.medium * 2
        LogosText {
            id: nbText
            anchors {
                left: parent.left
                right: parent.right
                verticalCenter: parent.verticalCenter
                leftMargin: Theme.spacing.medium
                rightMargin: Theme.spacing.medium
            }
            text: nb.text
            wrapMode: Text.Wrap
            color: Theme.palette.text
            font.pixelSize: Theme.typography.secondaryText
        }
    }

    component StepRow: RowLayout {
        id: stepr
        property int num: 1
        property string text: ""
        Layout.fillWidth: true
        spacing: Theme.spacing.small
        Rectangle {
            Layout.alignment: Qt.AlignTop
            implicitWidth: 20
            implicitHeight: 20
            radius: 10
            color: Theme.colors.getColor(Theme.palette.primary, 0.16)
            border.color: Theme.palette.primary
            border.width: 1
            LogosText {
                anchors.centerIn: parent
                text: stepr.num
                color: Theme.palette.primary
                font.pixelSize: 10
                font.weight: Theme.typography.weightBold
            }
        }
        LogosText {
            Layout.fillWidth: true
            text: stepr.text
            wrapMode: Text.Wrap
            color: Theme.palette.textSecondary
            font.pixelSize: Theme.typography.secondaryText
        }
    }

    // Analogue snow for the empty screen. Six small tileable frames cycled at
    // random beats the alternatives: a shader needs precompiling for Qt 6, and
    // generating noise per pixel in a Canvas costs more CPU than an idle screen
    // deserves. Kept faint — it should read as "no signal", not as decoration.
    component StaticNoise: Item {
        id: snow
        clip: true
        Image {
            id: grain
            property int frame: 1
            source: "assets/noise" + frame + ".png"
            fillMode: Image.Tile
            smooth: false
            // Oversized so the random offset never exposes an edge.
            x: -64
            y: -64
            width: snow.width + 128
            height: snow.height + 128
            opacity: 0.16
        }
        Timer {
            interval: 70
            running: snow.visible
            repeat: true
            onTriggered: {
                grain.frame = 1 + Math.floor(Math.random() * 6);
                grain.x = -64 + Math.floor(Math.random() * 64);
                grain.y = -64 + Math.floor(Math.random() * 64);
            }
        }
        // A band drifting down the screen, the way an untuned set rolls.
        Rectangle {
            width: snow.width
            height: 110
            opacity: 0.05
            gradient: Gradient {
                GradientStop {
                    position: 0.0
                    color: "transparent"
                }
                GradientStop {
                    position: 0.5
                    color: "#FFFFFF"
                }
                GradientStop {
                    position: 1.0
                    color: "transparent"
                }
            }
            NumberAnimation on y {
                running: snow.visible
                from: -110
                to: snow.height
                duration: 7000
                loops: Animation.Infinite
            }
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
        id: dotRow
        property bool live: false
        spacing: 6
        Rectangle {
            width: 8
            height: 8
            radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: parent.live ? Theme.palette.error : Theme.palette.textTertiary
            SequentialAnimation on opacity {
                running: dotRow.live
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
                source: "../icons/beacon.png"
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                sourceSize: Qt.size(128, 128)
                smooth: true
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
                        visible: root.playerUrl.length > 0
                        url: root.playerUrl
                    }

                    StaticNoise {
                        anchors.fill: parent
                        anchors.margins: 1
                        visible: !view.visible
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
                                Layout.alignment: Qt.AlignVCenter
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
            //
            // Two columns in one slot, because the tabs want opposite things:
            // Watch has Live now stretch to the bottom and scroll inside
            // itself, while Broadcast stacks cards at their natural height and
            // scrolls the column when they outgrow the window.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 2
                Layout.maximumWidth: 400

            ColumnLayout {
                anchors.fill: parent
                visible: root.tab === 0
                spacing: Theme.spacing.large

                // Watch — the key field first, then Live now filling what is
                // left. The list scrolls inside its own card, so a busy network
                // never pushes the field off the panel.
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

                Card {
                    visible: root.tab === 0
                    title: "Live now"
                    Layout.fillHeight: true
                    Layout.fillWidth: true

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: Theme.spacing.small
                        model: root.directory
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: LogosScrollBar {}

                        delegate: Rectangle {
                            width: ListView.view.width
                            height: 54
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

            }

            Flickable {
                id: bcFlick
                anchors.fill: parent
                visible: root.tab === 1
                contentHeight: bcColumn.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: LogosScrollBar {}

                ColumnLayout {
                    id: bcColumn
                    width: parent.width
                    // At least as tall as the panel, so the last card can reach
                    // the bottom; taller when the cards need it, and then the
                    // column scrolls.
                    height: Math.max(implicitHeight, bcFlick.height)
                    spacing: Theme.spacing.large

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
                            RowLayout {
                                Layout.fillWidth: true
                                visible: !root.broadcasting
                                spacing: Theme.spacing.small
                                LogosSwitch {
                                    id: listedSwitch
                                    checked: prefs.listed
                                    onCheckedChanged: prefs.listed = checked
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    LogosText {
                                        text: prefs.listed ? "Listed publicly" : "Unlisted"
                                        color: Theme.palette.text
                                        font.pixelSize: Theme.typography.secondaryText
                                        font.weight: Theme.typography.weightMedium
                                    }
                                    LogosText {
                                        Layout.fillWidth: true
                                        text: prefs.listed
                                            ? "Anyone running Beacon sees this station in Live now."
                                            : "Only people you give the key to can find it."
                                        wrapMode: Text.Wrap
                                        color: Theme.palette.textTertiary
                                        font.pixelSize: 11
                                    }
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

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.topMargin: Theme.spacing.small
                                    implicitHeight: 1
                                    color: Theme.palette.borderHairline
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Chip {
                                        label: root.st.listed === false ? "Unlisted" : "Listed"
                                        tint: root.st.listed === false ? Theme.palette.textSecondary : Theme.palette.primary
                                        implicitHeight: 22
                                    }
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
                            title: "Sending video"
                            Layout.alignment: Qt.AlignTop
                            StepRow {
                                num: 1
                                text: "OBS ▸ Settings ▸ Output, set Output Mode to Advanced."
                            }
                            StepRow {
                                num: 2
                                text: "Recording tab: Type \"Custom Output (FFmpeg)\", FFmpeg Output Type \"Output to URL\"."
                            }
                            StepRow {
                                num: 3
                                text: "URL " + (root.ingest.url && root.ingest.url.length ? root.ingest.url : "udp://127.0.0.1:9911") + ", container mpegts, encoders libx264 and aac."
                            }
                            StepRow {
                                num: 4
                                text: "Press Start Recording — not Start Streaming."
                            }
                            LogosText {
                                Layout.fillWidth: true
                                text: "The address never changes, so this is a one-time setup. Anything that speaks MPEG-TS works: ffmpeg, vMix, a phone encoder."
                                wrapMode: Text.Wrap
                                color: Theme.palette.textTertiary
                                font.pixelSize: 11
                            }
                        }

                        Card {
                            visible: root.tab === 1
                            title: "Your station key"
                            // Takes whatever height is left, so the column ends
                            // flush with the video beside it.
                            Layout.fillHeight: true
                            Layout.minimumHeight: implicitHeight
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

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 1
                                color: Theme.palette.borderHairline
                            }

                            MiniButton {
                                label: root.showKeyTools ? "Hide backup" : "Back up or restore this station"
                                onClicked: root.showKeyTools = !root.showKeyTools
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: root.showKeyTools
                                spacing: Theme.spacing.small

                                NoticeBanner {
                                    tint: Theme.palette.warning
                                    text: "A backup is the station itself. Anyone who holds it can broadcast as you, and a key cannot be revoked. Keep it somewhere private, and lose it and your audience's key is dead."
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    MiniButton {
                                        label: root.copied === "backup" ? "✓ Copied to clipboard" : "Copy backup"
                                        onClicked: root.exportStation(function (hex) {
                                            if (hex.length)
                                                root.copy(hex, "backup");
                                            else
                                                root.lastError = "No station key to back up.";
                                        })
                                    }
                                    Item {
                                        Layout.fillWidth: true
                                    }
                                }
                                FieldLabel {
                                    text: "Restore"
                                }
                                LogosTextField {
                                    id: importField
                                    Layout.fillWidth: true
                                    implicitHeight: 40
                                    placeholderText: "Paste a backup (128 hex characters)"
                                }
                                ActionButton {
                                    Layout.fillWidth: true
                                    text: "Replace this station"
                                    danger: true
                                    enabled: importField.text.trim().length === 128 && !root.broadcasting
                                    onClicked: root.importStation(importField.text)
                                }
                            }

                            // The card stretches to the bottom of the panel;
                            // the slack belongs here, not spread between rows.
                            Item {
                                Layout.fillHeight: true
                            }
                        }

                }
            }
            }
        }
    }
}
