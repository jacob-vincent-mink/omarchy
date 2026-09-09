import QtQuick
import QtQuick.Window
import Quickshell
import Quickshell.Wayland
import Omarchy.Ward
import qs.Commons
import "../PluginInput.js" as PluginInput

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
  onBarPlacementChanged: Qt.callLater(updateMask)
  property var barOwner: null
  readonly property size widgetSize: view.widgetSize
  property var panelCommand: null
  readonly property bool opened: !error && (panelCommand && panelCommand.serial !== view.panelSerial
    ? panelCommand.open : view.panelOpen)
  property bool focusPrimed: false
  property bool programmaticFocus: false
  onProgrammaticFocusChanged: Qt.callLater(updateMask)
  property bool focusHeld: false
  // Only host interaction can establish focusHeld. Worker-reported panel state
  // may retain that focus, but cannot independently capture the desktop.
  readonly property bool panelFocus: focusHeld && opened
  onPanelFocusChanged: Qt.callLater(updateMask)
  property bool hadWindowFocus: false
  onOpenedChanged: if (!opened) { focusHeld = false; programmaticFocus = false }
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
  signal panelSwitchRequested(int direction)
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
  WlrLayershell.keyboardFocus: focusPrimed || programmaticFocus ? WlrKeyboardFocus.Exclusive
    : focusHeld ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None
  mask: inputMask

  property Region inputMask: Region {}
  property Component regionComponent: Component { Region {} }
  property var regions: []
  function updateMask() {
    var previous = regions
    var next = []
    var source = programmaticFocus || panelFocus ? [{x: 0, y: 0, width: width, height: height}] : view.inputRegions
    var rectangles = PluginInput.barMask(source, barPlacement, width, height)
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
  function primeFocus(programmatic) {
    programmaticFocus = programmatic === true
    focusHeld = true
    // Re-priming an already focused Wayland layer can cancel a held pointer
    // gesture. Existing panel interaction needs no compositor focus handoff.
    if (!view.Window.active) {
      focusPrimed = true
      focusPrimeTimer.restart()
    }
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
      // Unlike a click, an IPC summon has no pointer activation that retains
      // OnDemand focus. Hold it until interaction or host-owned dismissal.
      primeFocus(true)
    } else {
      focusPrimeTimer.stop()
      focusPrimed = false
      programmaticFocus = false
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
  onWidthChanged: { Qt.callLater(configure); Qt.callLater(updateMask) }
  onHeightChanged: { Qt.callLater(configure); Qt.callLater(updateMask) }
  onRenderScaleChanged: Qt.callLater(configure)
  Component.onCompleted: Qt.callLater(configure)
  Component.onDestruction: view.stop()

  PluginView {
    id: view
    anchors.fill: parent
    onStateChanged: root.updateMask()
    onFocusRequested: root.primeFocus()
    onPanelSwitchRequested: direction => {
      if (root.opened && root.focusHeld) root.panelSwitchRequested(direction)
    }
    Window.onActiveChanged: {
      if (Window.active) root.hadWindowFocus = true
      else if (root.hadWindowFocus) {
        root.hadWindowFocus = false
        root.dismiss()
      }
    }
  }

  // Keep pointer transit between the bar and panel on the focused host surface.
  // The host owns outside-click dismissal; only clicks inside the existing
  // bounded worker regions reach the worker, and neighboring bar slots stay free.
  MouseArea {
    anchors.fill: parent
    enabled: root.programmaticFocus || root.panelFocus
    acceptedButtons: Qt.AllButtons
    onPressed: mouse => {
      const regions = PluginInput.barMask(view.inputRegions, root.barPlacement, root.width, root.height)
      if (regions.some(rect => mouse.x >= rect.x && mouse.y >= rect.y
        && mouse.x < rect.x + rect.width && mouse.y < rect.y + rect.height)) mouse.accepted = false
      else root.dismiss()
    }
  }
}
