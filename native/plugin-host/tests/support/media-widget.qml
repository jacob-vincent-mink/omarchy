import QtQuick
import Quickshell
import Quickshell.Services.Mpris
import qs.Ui as Ui
import qs.Commons
import "MediaModel.js" as MediaModel

// All objects below live inside the worker. This is a selected-player port of
// the media widget, not the shell's PipeWire service or a host QObject facade.
ShellRoot {
  id: root
  QtObject {
    id: media
    readonly property var sourcePlayers: Mpris.players.values
    readonly property var activePlayer: sourcePlayers.length === 1 ? sourcePlayers[0] : null
    function playerKey(player) { return MediaModel.playerKey(player) }
    function selectPlayer(key) { return activePlayer && playerKey(activePlayer) === key }
    function runAction(action, showOsd, key) {
      var player = activePlayer
      if (!player || (key && key !== playerKey(player)) || !MediaModel.canHandleAction(player, action)) return
      if (action === "playPause") player.togglePlaying()
      else if (action === "next") player.next()
      else if (action === "previous") player.previous()
    }
  }
  QtObject {
    id: localBar
    readonly property bool vertical: false
    readonly property string position: "top"
    readonly property int barSize: 36
    readonly property color foreground: Color.foreground
    readonly property color barForeground: foreground
    readonly property string fontFamily: Style.font.family
    readonly property bool foregroundAnimationEnabled: true
    readonly property QtObject shell: QtObject {
      function firstPartyServiceFor(id) { return id === "omarchy.media" ? media : null }
    }
    property var activePopout: null
    property string tooltipText: ""
    function requestPopout(owner) {
      if (activePopout && activePopout !== owner) activePopout.close()
      activePopout = owner
    }
    function releasePopout(owner) { if (activePopout === owner) activePopout = null }
    function showTooltip(owner, text) { localBar.tooltipText = text; tooltipDelay.restart() }
    function hideTooltip(owner) { tooltipDelay.stop(); tooltip.open = false }
  }
  Timer {
    id: tooltipDelay
    interval: 400
    onTriggered: if (!widget.popupOpen) tooltip.open = localBar.tooltipText !== ""
  }
  PanelWindow {
    anchors { top: true; left: true; right: true }
    implicitHeight: localBar.barSize
    color: Color.background
    MediaWidget {
      id: widget
      x: 180
      height: localBar.barSize
      bar: localBar
      onPopupOpenChanged: if (popupOpen) localBar.hideTooltip(widget)
    }
  }
  Ui.PopupCard {
    id: tooltip
    bar: localBar
    anchorItem: widget
    triggerMode: "hover"
    // Hover help must withdraw immediately when the media popup takes over.
    visible: open
    contentWidth: fittedContentWidth(Math.ceil(tooltipLabel.implicitWidth) + padding * 2 + Border.left(borderSpec) + Border.right(borderSpec))
    contentHeight: fittedContentHeight(tooltipLabel.implicitHeight)
    Text {
      id: tooltipLabel
      text: localBar.tooltipText
      textFormat: Text.PlainText
      color: Color.foreground
      font.family: Style.font.family
      font.pixelSize: Style.font.body
      elide: Text.ElideRight
      width: parent.width
    }
  }
  // Test-owned telemetry is separate from the unmodified widget. It lets the
  // headless test wait for actual native MPRIS and popup state between inputs.
  PanelWindow {
    anchors { bottom: true; left: true; right: true }
    implicitHeight: 24
    color: "#263442"
    Row {
      x: 8; y: 4; spacing: 8
      Rectangle { width: 16; height: 16; color: media.activePlayer && media.activePlayer.dbusName === "org.mpris.MediaPlayer2.Selected" ? "#44ee22" : "#ff3300" }
      Rectangle { width: 16; height: 16; color: media.activePlayer && media.activePlayer.isPlaying ? "#33cc88" : "#3388ff" }
      Rectangle { width: 16; height: 16; color: widget.popupOpen ? "#bb6633" : "#7799cc" }
      Rectangle { width: 16; height: 16; color: !media.activePlayer || media.activePlayer.trackTitle === "Sandbox track" ? "#ffee44" : media.activePlayer.trackTitle === "Sandbox track 1" ? "#ee5599" : "#ff7744" }
      Rectangle { width: 16; height: 16; color: tooltip.open ? (tooltipLabel.truncated ? "#ff00ff" : "#eedd99") : "#121212" }
      Text { text: media.activePlayer ? media.activePlayer.trackTitle : "No approved player"; color: "white"; font.pixelSize: 12 }
    }
  }
}
