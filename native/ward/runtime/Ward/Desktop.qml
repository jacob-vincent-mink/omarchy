pragma Singleton

import QtQuick
import Quickshell
import Quickshell.Io

// Explicit read-only compatibility view, not Quickshell.Hyprland. Both files
// are controller-authored, read-only mounts. No compositor socket is opened.
QtObject {
  id: root
  readonly property bool granted: permission.selected
  readonly property bool available: granted && context.snapshot !== null
  readonly property var toplevels: ({ values: available ? context.snapshot.windows.map(window => ({
    address: String(window.id),
    workspace: window.workspace === null ? null : { id: window.workspace },
    lastIpcObject: {
      at: [window.rect.x, window.rect.y], size: [window.rect.width, window.rect.height],
      mapped: window.mapped, hidden: window.hidden, fullscreen: window.fullscreen
    }
  })) : [] })

  // A notification to rebuild observations, not a raw compositor event log.
  signal changed()
  onToplevelsChanged: changed()

  function monitorFor(screen) {
    // The current worker has one private output. Host output names are never
    // exported; viewport explicitly associates that output with its host.
    if (!available || !screen || Quickshell.screens.length !== 1
      || screen !== Quickshell.screens[0]) return null
    const output = context.snapshot.outputs.find(output => output.id === context.snapshot.viewport)
    return {
      id: output.id, x: output.rect.x, y: output.rect.y,
      width: output.rect.width, height: output.rect.height, scale: output.scale,
      activeWorkspace: output.activeWorkspaces.length ? { id: output.activeWorkspaces[0] } : null,
      lastIpcObject: { reserved: output.reserved.slice() }
    }
  }

  property FileView permission: FileView {
    property bool selected: false
    path: "/run/plugin/grants.json"
    onLoaded: selected = JSON.parse(text()).desktopGeometry === true
    onLoadFailed: selected = false
  }
  property FileView context: FileView {
    property var snapshot: null
    path: "/context/state.json"
    watchChanges: true
    onFileChanged: reload()
    onLoaded: {
      const next = JSON.parse(text()).geometry || null
      if (JSON.stringify(next) !== JSON.stringify(snapshot)) snapshot = next
    }
    onLoadFailed: snapshot = null
  }
}
