import QtQuick
import Quickshell.Io

// Host presentation for authenticated Ward decisions. Worker notification
// grants and worker-provided messages cannot synthesize this signal.
QtObject {
  id: root
  required property var manager
  property double windowStarted: 0
  property int delivered: 0
  property var lastEvent: null
  property Connections events: Connections {
    target: root.manager
    function onBlocked(pluginId, action) { root.report(pluginId, action) }
  }

  function description(action) {
    const actions = {
      1: "sending an unapproved notification",
      2: "changing settings outside its approved access",
      3: "opening an unapproved web link",
      4: "making a request outside its approved HTTP access",
      5: "executing an unapproved host command"
    }
    return actions[action] || ""
  }

  function report(id, action) {
    if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$/.test(id) || id.indexOf("..") !== -1) return false
    const detail = description(action)
    if (!detail) return false
    const now = Date.now()
    lastEvent = {id: id, action: action, timestamp: now}
    // Also bound the desktop-wide notification rate across plugin sessions.
    if (now - windowStarted >= 30000) { windowStarted = now; delivered = 0 }
    if (delivery.running || delivered >= 2) return false
    delivered++
    delivery.command = ["timeout", "2s", "omarchy-notification-send", "--app-name", "Omarchy Ward",
      "-u", "normal", "-t", "6000", "Blocked " + id, "Ward blocked this plugin from " + detail + "."]
    delivery.running = true
    return true
  }

  property Process delivery: Process {}
}
