import QtQuick
import QtQuick.Window
import Quickshell
import Quickshell.Wayland
import Omarchy.Ward
import qs.Commons

// One private desktop canvas, hosted by the existing trusted shell. A worker's
// layer-shell requests affect only its private display, never this window.
PanelWindow {
  id: root
  required property string pluginId
  required property string store
  required property string controller
  property var settings: ({})
  property var geometrySource: null
  property var barPlacement: null
  property var barOwner: null
  readonly property size widgetSize: view.widgetSize
  property var panelCommand: null
  readonly property bool opened: !error && (panelCommand && panelCommand.serial !== view.panelSerial
    ? panelCommand.open : view.panelOpen)
  property bool focusPrimed: false
  property bool focusHeld: false
  property bool hadWindowFocus: false
  onOpenedChanged: if (!opened) focusHeld = false
  readonly property string contextJson: {
    const context = {
      settings: settings,
      panel: panelCommand,
      bar: barPlacement,
      geometry: geometrySource ? geometrySource.forScreen(targetScreen) : null,
      theme: {
        foreground: String(Color.foreground), background: String(Color.background),
        accent: String(Color.accent), urgent: String(Color.urgent), muted: String(Color.muted),
        shellValues: Color.shellValues, cornerRadius: Style.cornerRadius,
        gapsOut: Style.gapsOut, fontFamily: Style.resolvedFontFamily
      }
    }
    let json = JSON.stringify(context)
    // Preserve settings/panel delivery if the complete geometry snapshot does
    // not fit beside them. Never send a partial window/output list.
    if (context.geometry && encodeURIComponent(json).replace(/%[0-9A-F]{2}/g, "x").length > 65536) {
      context.geometry = null
      json = JSON.stringify(context)
    }
    return json
  }
  function updateContext() { if (started && !error) view.setContext(contextJson) }
  onContextJsonChanged: Qt.callLater(updateContext)
  readonly property string error: view.error
  readonly property string state: error ? "error" : view.presented ? "running" : "starting"
  signal statusChanged()
  onStateChanged: statusChanged()

  // The preview provides one desktop canvas, on the largest logical output.
  readonly property var targetScreen: {
    let largest = null
    for (const candidate of Quickshell.screens) {
      if (!largest || candidate.width * candidate.height > largest.width * largest.height)
        largest = candidate
    }
    return largest
  }
  screen: targetScreen
  anchors { top: true; bottom: true; left: true; right: true }
  color: "transparent"
  visible: !error && targetScreen !== null
  exclusionMode: ExclusionMode.Ignore
  WlrLayershell.namespace: "omarchy-ward-" + pluginId
  // Above the bar even when a replacement bar maps after this worker. The
  // bounded mask still leaves every unpainted/unclaimed area click-through.
  WlrLayershell.layer: WlrLayer.Overlay
  WlrLayershell.keyboardFocus: focusPrimed ? WlrKeyboardFocus.Exclusive
    : focusHeld ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None
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
  function primeFocus() {
    focusHeld = true
    focusPrimed = true
    focusPrimeTimer.restart()
    view.forceActiveFocus()
  }
  function setPanel(open, payload) {
    if (error) return false
    var text = open ? String(payload || "") : ""
    try {
      if (encodeURIComponent(text).replace(/%[0-9A-F]{2}/g, "x").length > 4096) return false
    } catch (e) { return false }
    panelCommand = {
      serial: (panelCommand ? panelCommand.serial : 0) % 2147483647 + 1,
      open: open, payload: text
    }
    if (open) {
      primeFocus()
    } else {
      focusPrimeTimer.stop()
      focusPrimed = false
      focusHeld = false
      view.dismiss()
    }
    return true
  }
  function dismiss() { return setPanel(false, "") }
  function close() { dismiss() }
  Timer { id: focusPrimeTimer; interval: 75; onTriggered: root.focusPrimed = false }
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
    onFocusRequested: root.primeFocus()
    Window.onActiveChanged: {
      if (Window.active) root.hadWindowFocus = true
      else if (root.hadWindowFocus) {
        root.hadWindowFocus = false
        root.dismiss()
      }
    }
  }
}
