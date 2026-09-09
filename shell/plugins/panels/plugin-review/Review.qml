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
  property var http: []
  property var exec: ({})
  readonly property var execRequests: {
    if (!revision) return []
    var rows = []
    function argument(arg) {
      if (arg.kind === "exact") return JSON.stringify(arg.value)
      if (arg.kind === "oneOf") return "one of " + JSON.stringify(arg.values)
      if (arg.kind === "integer") return "integer " + arg.min + "…" + arg.max
      if (arg.kind === "pattern") return "whole-argument pattern (≤" + arg.max + " bytes): " + arg.value
      return "text " + arg.min + "…" + arg.max + " bytes, prefix " + JSON.stringify(arg.prefix)
    }
    function walk(name, ask, tree, args) {
      if (tree.end) rows.push({name: name, leaf: tree.end, executable: ask.executable,
        required: ask.required.indexOf(tree.end) !== -1, command: args.join("\n") || "(no arguments)"})
      for (var step of tree.next) walk(name, ask, step.then, args.concat([argument(step.arg)]))
    }
    for (var name of Object.keys(revision.requests.exec)) {
      var ask = revision.requests.exec[name]
      walk(name, ask, ask.tree, [])
    }
    return rows
  }
  readonly property var httpRequests: revision ? Object.keys(revision.requests.http) : []
  property bool notifications: false
  property var settings: ({read: [], write: []})
  property bool openUrls: false
  property bool storage: false
  property bool desktopGeometry: false
  property string media: ""
  property var folders: ({})
  property var writableFolders: ({})
  readonly property var folderRequests: revision ? revision.requests.filesystem : []
  readonly property var settingRequests: {
    if (!revision) return []
    var result = []
    for (var access of ["read", "write"])
      for (var key of revision.requests.settings[access]) result.push({access: access, key: key})
    return result
  }
  property bool busy: false
  property string operation: ""
  property string error: ""
  property string notice: ""
  property bool selectionApproved: false

  onNetworkChanged: { selectionApproved = false; if (network) http = [] }
  onHttpChanged: selectionApproved = false
  onExecChanged: selectionApproved = false
  onNotificationsChanged: selectionApproved = false
  onSettingsChanged: selectionApproved = false
  onOpenUrlsChanged: selectionApproved = false
  onStorageChanged: selectionApproved = false
  onDesktopGeometryChanged: selectionApproved = false
  onMediaChanged: selectionApproved = false
  onFoldersChanged: selectionApproved = false
  onWritableFoldersChanged: selectionApproved = false

  function requestLabel(name, label) {
    var request = revision ? revision.requests[name] : null
    return label + (request && request.required === true ? " · required" : "")
  }

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
    http = []
    exec = ({})
    notifications = false
    settings = ({read: [], write: []})
    openUrls = false
    storage = false
    desktopGeometry = false
    media = ""
    folders = ({})
    writableFolders = ({})
    selectionApproved = false
    return run("review", ["omarchy-plugin-review", id, "--json"])
  }

  function setFolder(slot, path) {
    var next = Object.assign({}, folders)
    next[slot] = path
    folders = next
  }

  function setWritable(slot, allowed) {
    var next = Object.assign({}, writableFolders)
    next[slot] = allowed === true
    writableFolders = next
  }

  function toggleSetting(access, key) {
    var next = {read: settings.read.slice(), write: settings.write.slice()}
    var index = next[access].indexOf(key)
    if (index < 0) next[access].push(key)
    else next[access].splice(index, 1)
    settings = next
  }

  function toggleHttp(name) {
    var next = http.slice()
    var index = next.indexOf(name)
    if (index < 0) { next.push(name); network = false }
    else next.splice(index, 1)
    http = next
  }

  function toggleExec(name, leaf) {
    var next = Object.assign({}, exec)
    var selected = (Object.prototype.hasOwnProperty.call(exec, name) ? exec[name] : []).slice()
    var index = selected.indexOf(leaf)
    if (index < 0) selected.push(leaf)
    else selected.splice(index, 1)
    next[name] = selected
    exec = next
  }

  function approve() {
    if (!revision || busy) return
    var args = ["omarchy-plugin-approve", pluginId, "--revision", revision.revision, "--yes"]
    if (network) args.push("--allow-network")
    for (var name of http) args.push("--http", name)
    for (var name of Object.keys(exec))
      for (var leaf of exec[name]) args.push("--exec", name + ":" + leaf)
    if (notifications) args.push("--allow-notifications")
    for (var access of ["read", "write"])
      for (var key of settings[access]) args.push("--" + access + "-setting", key)
    if (openUrls) args.push("--allow-open-urls")
    if (storage) args.push("--allow-storage")
    if (desktopGeometry) args.push("--allow-desktop-geometry")
    if (media.trim()) args.push("--media", media.trim())
    for (var slot in folders) if (folders[slot].trim())
      args.push(writableFolders[slot] === true ? "--write" : "--read", slot + "=" + folders[slot].trim())
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
