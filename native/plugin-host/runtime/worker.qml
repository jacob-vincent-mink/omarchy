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

  FileView {
    path: "/plugin/manifest.json"
    onLoaded: {
      root.manifest = JSON.parse(text())
      var entries = root.manifest.entryPoints
      if (entries.service) serviceLoader.source = root.entryUrl(entries.service)
      if (entries.overlay) overlayLoader.source = root.entryUrl(entries.overlay)
      widgetLoader.setSource(root.entryUrl(entries.barWidget), { bar: barApi, settings: {} })
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
