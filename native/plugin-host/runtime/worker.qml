import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import qs.Commons
import qs.Ui
import "Services"

// Evaluated only inside the restricted worker, never by the desktop shell.
ShellRoot {
  id: root
  property var manifest: null
  property var context: null
  property bool loaded: false
  property string pendingSettings: ""
  property string settingsError: ""
  readonly property var panel: overlayLoader.item || widgetLoader.item
  readonly property bool opened: panel && panel.opened === true
  readonly property string section: manifest && manifest.barWidget
    ? (manifest.barWidget.defaultSection || "center") : "center"

  function entryUrl(path) {
    return "file:///plugin/" + path.split("/").map(encodeURIComponent).join("/")
  }

  function inject(item) {
    if ("shell" in item) item.shell = shellApi
    if ("manifest" in item) item.manifest = manifest
    if ("omarchyPath" in item) item.omarchyPath = Quickshell.env("OMARCHY_PATH")
  }

  function checkLoader(loader) {
    if (loader.status === Loader.Error) {
      console.error("Shared plugin entry failed: " + loader.source)
      Qt.quit()
    }
  }

  function loadEntries() {
    if (loaded || !manifest || !context) return
    loaded = true
    var entries = manifest.entryPoints
    if (entries.service) serviceLoader.source = entryUrl(entries.service)
    if (entries.overlay) overlayLoader.source = entryUrl(entries.overlay)
    widgetLoader.setSource(entryUrl(entries.barWidget), { bar: barApi, settings: JSON.parse(JSON.stringify(context.settings)) })
  }

  FileView {
    path: "/context/state.json"
    watchChanges: true
    onFileChanged: reload()
    onLoaded: {
      root.context = JSON.parse(text())
      var theme = root.context.theme
      if (theme) {
        Color.foreground = theme.foreground
        Color.background = theme.background
        Color.accent = theme.accent
        Color.urgent = theme.urgent
        Color.muted = theme.muted
        Color.shellValues = theme.shellValues
        Style.applyShellValues(theme.shellValues)
        Style.cornerRadius = theme.cornerRadius
        Style.gapsOut = theme.gapsOut
        Style.resolvedFontFamily = theme.fontFamily
      }
      if (widgetLoader.item) widgetLoader.item.settings = JSON.parse(JSON.stringify(root.context.settings))
      root.loadEntries()
    }
    onLoadFailed: Qt.quit()
  }

  FileView {
    path: "/plugin/manifest.json"
    onLoaded: {
      root.manifest = JSON.parse(text())
      root.loadEntries()
    }
    onLoadFailed: Qt.quit()
  }

  PluginShellApi {
    id: shellApi
    pluginId: root.manifest ? root.manifest.id : ""
    _serviceLookup: id => id === pluginId ? serviceLoader.item : null
    _summon: (id, payload) => {
      if (id !== pluginId || !root.panel || typeof root.panel.open !== "function") return false
      root.panel.open(payload)
      return true
    }
    _hide: id => {
      if (id !== pluginId || !root.panel || typeof root.panel.close !== "function") return false
      root.panel.close()
      return true
    }
    _toggle: (id, payload) => root.opened ? _hide(id) : _summon(id, payload)
    _isOpen: id => id === pluginId && root.opened
    _updateSettings: (id, settings) => {
      if (id !== pluginId || !settings || typeof settings !== "object" || Array.isArray(settings)) return false
      root.pendingSettings = JSON.stringify(settings)
      settingsTimer.restart()
      return true
    }
  }

  Timer {
    id: settingsTimer
    interval: 300
    onTriggered: {
      if (settingsProcess.running || !root.pendingSettings) return
      settingsProcess.command = ["/bootstrap", "--settings", root.pendingSettings]
      root.pendingSettings = ""
      root.settingsError = ""
      settingsProcess.running = true
    }
  }
  Process {
    id: settingsProcess
    onExited: function(code) {
      if (code !== 0) {
        root.settingsError = "Settings were not saved. Review plugin access or retry."
        if (widgetLoader.item) widgetLoader.item.settings = JSON.parse(JSON.stringify(root.context.settings))
        errorTimer.restart()
      }
      if (root.pendingSettings) settingsTimer.restart()
    }
  }
  Timer { id: errorTimer; interval: 5000; onTriggered: root.settingsError = "" }
  PanelWindow {
    visible: root.settingsError !== ""
    anchors { bottom: true; right: true }
    margins { bottom: Style.gapsOut; right: Style.gapsOut }
    implicitWidth: Math.min(Style.space(340), screen ? screen.width - 2 * Style.gapsOut : 340)
    implicitHeight: saveError.implicitHeight + 2 * Style.spacing.popupPadding
    color: "transparent"
    exclusionMode: ExclusionMode.Ignore
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
    mask: Region {}
    BorderSurface {
      anchors.fill: parent
      color: Color.popups.background
      radius: Style.cornerRadius
      borderSpec: Border.surfaceSpec("popups", "border", Color.popups.border, 1)
      Text {
        id: saveError
        anchors.centerIn: parent
        width: parent.width - 2 * Style.spacing.popupPadding
        text: root.settingsError
        color: Color.popups.text
        font.family: Style.font.family
        font.pixelSize: Style.font.body
        wrapMode: Text.WordWrap
      }
    }
  }

  Item {
    visible: false
    Loader {
      id: serviceLoader
      onLoaded: root.inject(item)
      onStatusChanged: root.checkLoader(this)
    }
    Loader {
      id: overlayLoader
      onLoaded: root.inject(item)
      onStatusChanged: root.checkLoader(this)
    }
  }

  PluginBarApi {
    id: barApi
    pluginId: shellApi.pluginId
    moduleName: pluginId
    shell: shellApi
    foreground: Color.bar.text
    barForeground: Color.bar.text
    background: Color.bar.background
    urgent: Color.urgent
    fontFamily: Style.font.family
    barSize: Style.bar.sizeHorizontal
    _registerClickTarget: target => { clickTargets = clickTargets.concat([target]) }
    _unregisterClickTarget: target => { clickTargets = clickTargets.filter(value => value !== target) }
    _requestPopout: owner => { activePopout = owner }
    _releasePopout: owner => { if (activePopout === owner) activePopout = null }
    _targetBelongsToWindow: (target, window) => target.QsWindow.window === window
    _moduleWidgets: id => id === pluginId && widgetLoader.item ? [widgetLoader.item] : []
    _setCenterHoverRevealSuppressed: value => { _centerHoverRevealSuppressed = value }
  }

  // Initial single-screen preview placement; host bar-slot integration follows.
  PanelWindow {
    anchors { top: true; left: true; right: true }
    implicitHeight: barApi.barSize
    color: "transparent"
    exclusionMode: ExclusionMode.Ignore
    WlrLayershell.layer: WlrLayer.Top
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
    WlrLayershell.namespace: "omarchy-private-bar"
    mask: Region { item: widgetLoader }

    Loader {
      id: widgetLoader
      x: root.section === "left" ? Style.gapsOut
        : root.section === "right" ? parent.width - width - Style.gapsOut
        : (parent.width - width) / 2
      anchors.verticalCenter: parent.verticalCenter
      width: item ? item.implicitWidth : 0
      height: item ? item.implicitHeight : 0
      onStatusChanged: root.checkLoader(this)
    }
  }
}
