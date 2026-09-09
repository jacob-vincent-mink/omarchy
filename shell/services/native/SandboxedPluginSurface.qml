import QtQuick
import Quickshell
import Quickshell.Wayland
import Omarchy.PluginHost
import qs.Commons

// One private desktop canvas, hosted by the existing trusted shell. A worker's
// layer-shell requests affect only its private display, never this window.
PanelWindow {
  id: root
  required property string pluginId
  required property string store
  required property string controller
  property var settings: ({})
  readonly property string contextJson: JSON.stringify({
    settings: settings,
    theme: {
      foreground: String(Color.foreground), background: String(Color.background),
      accent: String(Color.accent), urgent: String(Color.urgent), muted: String(Color.muted),
      shellValues: Color.shellValues, cornerRadius: Style.cornerRadius,
      gapsOut: Style.gapsOut, fontFamily: Style.resolvedFontFamily
    }
  })
  function updateContext() { if (started && !error) view.setContext(contextJson) }
  onContextJsonChanged: Qt.callLater(updateContext)
  property bool shown: true
  readonly property string error: view.error
  readonly property string state: error ? "error" : view.presented ? "running" : "starting"
  signal statusChanged()
  onStateChanged: statusChanged()

  readonly property var targetScreen: Quickshell.screens[0] || null
  screen: targetScreen
  anchors { top: true; bottom: true; left: true; right: true }
  color: "transparent"
  visible: shown && !error && targetScreen !== null
  exclusionMode: ExclusionMode.Ignore
  WlrLayershell.namespace: "omarchy-plugin-" + pluginId
  WlrLayershell.layer: WlrLayer.Top
  WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand
  mask: inputMask

  property Region inputMask: Region {}
  property Component regionComponent: Component { Region {} }
  property var regions: []
  function updateMask() {
    var previous = regions
    var next = []
    var rectangles = view.inputRegions
    for (var i = 0; i < rectangles.length; i++) {
      var rect = rectangles[i]
      next.push(regionComponent.createObject(inputMask, {
        x: rect.x, y: rect.y, width: rect.width, height: rect.height
      }))
    }
    inputMask.regions = next
    regions = next
    for (var j = 0; j < previous.length; j++) previous[j].destroy()
  }

  function stop() { view.stop() }
  function dismiss() { view.dismiss() }
  readonly property int renderScale: targetScreen ? Math.max(1, Math.ceil(targetScreen.devicePixelRatio)) : 1
  property bool started: false
  function configure() {
    if (width <= 0 || height <= 0 || !targetScreen || error) return
    if (!started) {
      started = true
      view.start(store, pluginId, controller, width, height, renderScale, contextJson)
    } else {
      view.configure(width, height, renderScale)
    }
  }
  onWidthChanged: Qt.callLater(configure)
  onHeightChanged: Qt.callLater(configure)
  onRenderScaleChanged: Qt.callLater(configure)
  Component.onCompleted: Qt.callLater(configure)
  Component.onDestruction: view.stop()

  PluginView {
    id: view
    anchors.fill: parent
    onStateChanged: root.updateMask()
  }
}
