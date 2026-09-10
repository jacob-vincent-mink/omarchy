import QtQuick
import Quickshell.Io

// Presentation state only. Installation provenance and execution mode belong
// to the CLI, and exact-revision capability approval belongs to Ward.
QtObject {
  id: root
  property var plugins: []
  property string source: ""
  property bool yolo: false
  property bool trustConfirmed: false
  property var inspected: null
  property string selectedId: ""
  readonly property var selected: plugins.find(row => row.id === selectedId) || null
  property bool adding: false
  property bool confirmRemove: false
  property bool busy: false
  property string operation: ""
  property string error: ""
  property string notice: ""
  signal installed(string id, bool sandboxed)

  onSourceChanged: { inspected = null; trustConfirmed = false; error = "" }
  onYoloChanged: { inspected = null; trustConfirmed = false; error = "" }
  onSelectedIdChanged: confirmRemove = false

  function modeLabel(mode) {
    if (mode === "ward") return "Ward · sandboxed"
    if (mode === "yolo") return "YOLO · unsandboxed"
    if (mode === "trusted-local") return "Trusted local · unsandboxed"
    if (mode === "blocked") return "Blocked · check installation"
    return "Legacy trusted · unsandboxed"
  }

  function load(add) {
    if (busy) return false
    adding = !!add
    inspected = null
    trustConfirmed = false
    confirmRemove = false
    return refresh()
  }

  function refresh() { return run("list", ["omarchy-plugin-list", "--json"]) }

  function inspect() {
    if (!source.trim()) { error = "Enter a Git URL or local Git folder."; return false }
    inspected = null
    let args = ["omarchy-plugin-add", source.trim(), "--inspect", "--json"]
    if (yolo) args.push("--yolo")
    return run("inspect", args)
  }

  function add() {
    if (!inspected || (yolo && !trustConfirmed)) return false
    let args = ["omarchy-plugin-add", source.trim(), "--commit", inspected.commit, "--json", "--yes"]
    if (yolo) args.push("--yolo")
    return run("add", args)
  }

  function action(name) {
    if (!selected || ["enable", "disable", "remove"].indexOf(name) === -1) return false
    if (name === "remove" && !confirmRemove) { confirmRemove = true; return false }
    let args = ["omarchy-plugin-" + name, selected.id]
    if (name === "remove") args.push("--yes")
    return run(name, args)
  }

  function run(kind, args) {
    if (busy) return false
    busy = true
    operation = kind
    error = ""
    notice = ""
    command.command = args
    command.running = true
    return true
  }

  function finish(code, output, errors) {
    const kind = operation
    busy = false
    operation = ""
    if (code !== 0) {
      error = String(errors || output || "Command failed.").trim()
      return
    }
    try {
      if (kind === "list") {
        const rows = JSON.parse(output)
        if (!Array.isArray(rows)) throw new Error("Invalid plugin list")
        plugins = rows.filter(row => !row.firstParty)
      } else if (kind === "inspect") {
        const result = JSON.parse(output)
        if (!result.id || !result.commit || result.installed !== false) throw new Error("Invalid validation result")
        inspected = result
        notice = "Manifest validated. No plugin was installed or run. " + (yolo ? "YOLO will run unsandboxed when enabled." : "Ward access is reviewed after adding.")
      } else if (kind === "add") {
        const result = JSON.parse(output)
        if (!result.id || result.installed !== true) throw new Error("Invalid installation result")
        selectedId = result.id
        adding = false
        inspected = null
        trustConfirmed = false
        installed(result.id, result.mode === "ward")
        Qt.callLater(refresh)
      } else {
        confirmRemove = false
        Qt.callLater(refresh)
      }
    } catch (e) { error = "Could not read command result: " + e }
  }

  property Process command: Process {
    stdout: StdioCollector { id: output }
    stderr: StdioCollector { id: errors }
    onExited: (code, status) => root.finish(code, output.text, errors.text)
  }
}
