import QtQuick
import Quickshell.Io

// UI draft only. The existing commands and Rust store own revisions, grants,
// admission and revocation. Each action passes an argument vector, not a shell.
QtObject {
  id: root
  property string pluginId: ""
  property var revision: null
  property var current: null
  property bool network: false
  property bool notifications: false
  property string media: ""
  property var folders: ({})
  property bool busy: false
  property string operation: ""
  property string error: ""
  property string notice: ""
  property bool selectionApproved: false

  onNetworkChanged: selectionApproved = false
  onNotificationsChanged: selectionApproved = false
  onMediaChanged: selectionApproved = false
  onFoldersChanged: selectionApproved = false

  function load(id) {
    if (busy) return false
    if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(id) || id.indexOf("..") !== -1) {
      error = "Choose an installed plugin to review."
      return false
    }
    pluginId = id
    revision = null
    current = null
    network = false
    notifications = false
    media = ""
    folders = ({})
    selectionApproved = false
    return run("review", ["omarchy-plugin-review", id, "--json"])
  }

  function setFolder(slot, path) {
    var next = Object.assign({}, folders)
    next[slot] = path
    folders = next
  }

  function approve() {
    if (!revision || busy) return
    var args = ["omarchy-plugin-approve", pluginId, "--revision", revision.revision, "--yes"]
    if (network) args.push("--allow-network")
    if (notifications) args.push("--allow-notifications")
    if (media.trim()) args.push("--media", media.trim())
    for (var slot in folders) if (folders[slot].trim()) args.push("--read", slot + "=" + folders[slot].trim())
    run("approve", args)
  }

  function enable() {
    if (selectionApproved && !busy) run("enable", ["omarchy-plugin-enable", pluginId])
  }

  function revoke() {
    if (revision && !busy) run("disable", ["omarchy-plugin-disable", pluginId])
  }

  function run(kind, args) {
    if (busy) return false
    busy = true
    operation = kind
    error = ""
    if (kind !== "status") notice = ""
    process.out = ""
    process.err = ""
    process.outDone = false
    process.errDone = false
    process.exited = false
    process.command = ["timeout", "20s"].concat(args)
    process.running = true
    return true
  }

  function finish() {
    if (!busy || !process.exited || !process.outDone || !process.errDone) return
    var kind = operation
    busy = false
    if (process.exitCode !== 0) {
      error = process.err.trim() || (process.exitCode === 124 ? "Command timed out. Refresh to check its result." : "Command failed. Refresh to check its result.")
      return
    }
    try {
      if (kind === "review") {
        var info = JSON.parse(process.out)
        if (info.id !== pluginId || !/^[0-9a-f]{64}$/.test(info.revision) || !info.requests)
          throw new Error("Invalid review response")
        revision = info
        notice = "No code ran. Existing approvals remain unchanged until you approve or revoke."
      } else if (kind === "status") {
        var rows = JSON.parse(process.out)
        current = rows.find(function(row) { return row.id === root.pluginId }) || null
        return
      } else {
        selectionApproved = kind === "approve" || (kind === "enable" && selectionApproved)
        notice = kind === "approve" ? "Approval saved. No plugin was started."
          : kind === "enable" ? "Plugin enabled. You can close this review."
          : "Plugin disabled and access revoked."
      }
      run("status", ["omarchy-plugin-list", "--json"])
    } catch (e) {
      error = "Could not read the command result: " + e
    }
  }

  property Process process: Process {
    property string out: ""
    property string err: ""
    property bool outDone: false
    property bool errDone: false
    property bool exited: false
    property int exitCode: -1
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: { root.process.out = text; root.process.outDone = true; root.finish() }
    }
    stderr: StdioCollector {
      waitForEnd: true
      onStreamFinished: { root.process.err = text; root.process.errDone = true; root.finish() }
    }
    onExited: function(code) { exitCode = code; exited = true; root.finish() }
  }
}
