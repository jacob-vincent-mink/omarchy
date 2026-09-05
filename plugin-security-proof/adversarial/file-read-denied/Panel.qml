import QtQuick
import Omarchy.PluginPresentation 1.0

Item {
  id: root
  width: 520
  height: 260
  property var inputRegions: [{x: 0, y: 0, width: width, height: height}]

  FileReadProbe { id: probe }

  KeyboardPanel {
    contentWidth: 500
    contentHeight: 220
    radius: Style.cornerRadius
    border.width: Style.normalBorderWidth
    border.color: Color.alpha(Color.foreground, 0.25)

    Column {
      anchors.fill: parent
      anchors.margins: 24
      spacing: 14

      Text {
        text: probe.verdict === "BLOCKED" ? "File access blocked" : probe.verdict
        color: probe.verdict === "BREACH" ? Color.urgent : Color.foreground
        font.family: Style.font.family
        font.pixelSize: Style.font.heading
      }
      Text {
        text: "Permissions requested: none"
        color: Color.foreground
        font.family: Style.font.family
        font.pixelSize: Style.font.body
      }
      Text {
        text: "Direct file:///etc/passwd: " + probe.directResult
        color: Color.foreground
        font.family: Style.font.family
        font.pixelSize: Style.font.body
      }
      Text {
        text: "Packaged-text path traversal: " + probe.traversalResult
        color: Color.foreground
        font.family: Style.font.family
        font.pixelSize: Style.font.body
      }
    }
  }
}
