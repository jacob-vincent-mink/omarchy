import QtQuick
import Quickshell

// Host-owned layout spacer. No plugin source or worker QObject enters the bar.
Item {
  id: root
  required property var manager
  property var bar: null
  property string moduleName: ""
  property var settings: ({})
  readonly property var instance: manager.instances[moduleName] || null
  readonly property var window: QsWindow.window
  readonly property bool hosted: instance && window && window.screen === instance.targetScreen
  readonly property bool opened: instance && instance.opened
  function open() { return manager.show(moduleName, "") }
  function close() { return manager.hide(moduleName) }
  function closeForPopoutSwitch() { close() }
  implicitWidth: hosted ? instance.widgetSize.width : 0
  implicitHeight: hosted ? instance.widgetSize.height : 0

  TransformWatcher {
    id: positionWatcher
    a: root.window ? root.window.contentItem : null
    b: root
  }
  readonly property var placement: {
    positionWatcher.transform
    if (!hosted || !bar) return null
    const point = mapToItem(window.contentItem, 0, 0)
    const global = mapToGlobal(0, 0)
    const screen = window.screen
    const margins = "margins" in window ? window.margins : null
    const parked = margins && ((bar.position === "top" && margins.top <= -bar.barSize)
      || (bar.position === "bottom" && margins.bottom <= -bar.barSize)
      || (bar.position === "left" && margins.left <= -bar.barSize)
      || (bar.position === "right" && margins.right <= -bar.barSize))
    return {
      x: point.x, y: point.y, width: Math.min(1024, width), height: Math.min(1024, height),
      size: bar.barSize, position: bar.position,
      visible: visible && window.visible && !parked && global.x + width > screen.x && global.y + height > screen.y
        && global.x < screen.x + screen.width && global.y < screen.y + screen.height
    }
  }
  onPlacementChanged: Qt.callLater(publish)
  function publish() {
    if (hosted && placement) {
      const changedOwner = instance.barOwner !== root
      instance.barOwner = root
      instance.barPlacement = placement
      if (changedOwner) manager.coordinate(instance)
    }
  }
  Component.onDestruction: {
    if (instance && instance.barOwner === root) {
      instance.barOwner = null
      instance.barPlacement = Object.assign({}, instance.barPlacement, { visible: false })
    }
  }
}
