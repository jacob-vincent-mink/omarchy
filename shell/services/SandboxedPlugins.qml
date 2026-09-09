import QtQuick
import Quickshell
import Quickshell.Hyprland

// Trusted lifecycle adapter. It selects only a host-owned component; plugin
// paths and QML never enter this engine. Rust owns admission and supervision.
QtObject {
  id: root
  property var instances: ({})
  property var component: null
  property var bar: null
  property Connections workspaceChanges: Connections {
    target: Hyprland
    function onFocusedWorkspaceChanged() { root.dismissAll() }
    function onFocusedMonitorChanged() { root.dismissAll() }
  }
  property Connections popoutChanges: Connections {
    target: root.bar
    ignoreUnknownSignals: true
    function onActivePopoutChanged() {
      if (root.bar && root.bar.activePopout) root.dismissAll(root.bar.activePopout)
    }
  }
  property string store: Quickshell.env("OMARCHY_WARD_STORE")
    || (Quickshell.env("XDG_STATE_HOME") || Quickshell.env("HOME") + "/.local/state") + "/omarchy/ward"
  property string controller: Quickshell.env("OMARCHY_WARD_HOST")
    || Quickshell.env("OMARCHY_PATH") + "/lib/omarchy-ward"
  signal changed()
  signal activated(string pluginId)

  function status(id) {
    var instance = instances[id]
    if (!instance) return { state: "disabled", error: "" }
    return { state: instance.state, error: instance.error }
  }

  function ownSettings(entry) {
    var settings = JSON.parse(JSON.stringify(entry || {}))
    delete settings.id
    delete settings.sandbox
    return settings
  }

  function enable(id, entry) {
    if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(id) || id.indexOf("..") !== -1)
      return "invalid plugin id"
    var previous = instances[id]
    if (previous && previous.state !== "error") return previous.state === "running" ? "ok" : "starting"
    if (!component) component = Qt.createComponent("native/SandboxedPluginSurface.qml")
    if (component.status !== Component.Ready)
      return "native plugin host unavailable: " + component.errorString()
    if (previous) disable(id)
    if (Object.keys(instances).length >= 16) return "too many active sandbox plugins"
    var instance = component.createObject(root, {
      pluginId: id, store: store, controller: controller, settings: ownSettings(entry)
    })
    if (!instance) return "could not create native plugin host: " + component.errorString()
    var next = Object.assign({}, instances)
    next[id] = instance
    instances = next
    instance.statusChanged.connect(function() {
      if (instances[id] !== instance) return
      changed()
      if (instance.state === "running") activated(id)
    })
    var coordinate = function() {
      if (instances[id] !== instance) return
      // Worker-reported state alone cannot take over host popup ownership.
      if (instance.opened && instance.focusHeld) {
        dismissAll(instance)
        if (bar && typeof bar.requestPopout === "function") bar.requestPopout(instance)
      } else if (bar && typeof bar.releasePopout === "function") bar.releasePopout(instance)
    }
    instance.openedChanged.connect(coordinate)
    instance.focusHeldChanged.connect(coordinate)
    changed()
    return "starting"
  }

  function disable(id) {
    var instance = instances[id]
    if (!instance) return
    if (bar && typeof bar.releasePopout === "function") bar.releasePopout(instance)
    var next = Object.assign({}, instances)
    delete next[id]
    instances = next
    instance.stop()
    instance.destroy()
    changed()
  }

  function show(id, payload) {
    var instance = instances[id]
    if (!instance || instance.state === "error") return false
    return instance.setPanel(true, payload)
  }

  function hide(id) {
    var instance = instances[id]
    if (!instance) return false
    return instance.dismiss()
  }

  // Closing private panels must not unmap their persistent bar widgets.
  function dismissAll(except) {
    for (var id in instances) if (instances[id] !== except) instances[id].dismiss()
  }

  function isOpen(id) { return !!instances[id] && instances[id].opened }

  function sync(entries) {
    var desired = ({})
    for (var i = 0; i < entries.length; i++) {
      var entry = entries[i]
      if (entry && entry.sandbox === true) {
        desired[String(entry.id)] = true
        if (!instances[entry.id]) {
          var result = enable(String(entry.id), entry)
          if (result !== "starting" && result !== "ok") console.warn(result)
        } else instances[entry.id].settings = ownSettings(entry)
      }
    }
    for (var id in instances) if (!desired[id]) disable(id)
  }
}
