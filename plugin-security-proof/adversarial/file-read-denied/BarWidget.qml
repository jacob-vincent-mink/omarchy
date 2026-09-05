import QtQuick
import Omarchy.PluginPresentation 1.0

Item {
  id: root
  implicitWidth: Style.bar.statusSlot
  implicitHeight: Style.bar.size
  property var inputRegions: [{x: 0, y: 0, width: width, height: height}]

  FileReadProbe { id: probe }

  Text {
    anchors.centerIn: parent
    text: probe.verdict === "BREACH" ? "!" : "\uf3ed"
    color: probe.verdict === "BREACH" ? Color.urgent : Color.bar.text
    font.family: Style.font.family
    font.pixelSize: Style.font.icon
  }

  MouseArea {
    anchors.fill: parent
    onPressed: runtime.requestSurfaceIntent("panel", "toggle")
  }
}
