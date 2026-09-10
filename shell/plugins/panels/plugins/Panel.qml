import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import Quickshell
import Quickshell.Hyprland
import qs.Commons
import qs.Ui

Item {
  id: root
  property var shell: null
  property var manifest: null
  property bool opened: false
  readonly property alias model: manager

  function open(payloadJson) {
    let payload = {}
    try { payload = JSON.parse(payloadJson || "{}") || {} } catch (e) {}
    opened = true
    if (!manager.load(payload.add) && manager.busy) manager.error = "Wait for the current operation to finish."
  }
  function close() { opened = false }
  function dismiss() {
    if (shell) shell.hide("omarchy.plugins")
    else close()
  }
  function review(id) {
    if (shell) {
      dismiss()
      shell.summon("omarchy.plugin-review", JSON.stringify({id: id}))
    }
  }

  Model { id: manager; onInstalled: (id, sandboxed) => { if (sandboxed) root.review(id) } }

  KeyboardPanel {
    id: panel
    objectName: "plugin-manager-window"
    anchorItem: null
    bar: null
    owner: QtObject { function close() { root.dismiss() } }
    screen: Quickshell.screens.find(screen => screen.name === Hyprland.focusedMonitor?.name) || Quickshell.screens[0] || null
    open: root.opened
    focusTarget: content
    contentWidth: fittedContentWidth(Style.space(560))
    contentHeight: cappedContentHeight(Style.space(620))

    FocusScope {
      id: content
      anchors.fill: parent
      Keys.onEscapePressed: root.dismiss()

      ColumnLayout {
        anchors.fill: parent
        spacing: Style.spacing.panelGap

        RowLayout {
          Layout.fillWidth: true
          Label { text: manager.adding ? "Add plugin" : "Plugins"; font.pixelSize: Style.font.heading; font.bold: true; Layout.fillWidth: true }
          Button { text: "Close"; focusable: true; implicitHeight: 40; onClicked: root.dismiss() }
        }
        PanelSeparator { foreground: Color.popups.text; Layout.fillWidth: true }

        Controls.ScrollView {
          id: scroll
          Layout.fillWidth: true
          Layout.fillHeight: true
          clip: true
          rightPadding: Style.spacing.rowGap + effectiveScrollBarWidth
          contentWidth: availableWidth
          Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
          Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AlwaysOn

          Column {
            width: scroll.availableWidth
            spacing: Style.spacing.rowGap

            Column {
              visible: manager.adding
              width: parent.width
              spacing: Style.spacing.rowGap
              enabled: !manager.busy
              Label { text: "Git URL or local Git folder"; width: parent.width }
              TextField {
                objectName: "plugin-source"
                width: parent.width
                implicitHeight: 40
                text: manager.source
                placeholderText: "https://github.com/owner/plugin"
                onTextChanged: if (manager.source !== text) manager.source = text
              }
              Toggle {
                objectName: "plugin-yolo"
                width: parent.width
                label: "YOLO · run without a sandbox"
                description: "Off by default. Ward-compatible plugins are isolated and require separate permission approval."
                checked: manager.yolo
                onClicked: manager.yolo = !manager.yolo
              }
              Label {
                visible: manager.yolo
                width: parent.width
                text: "YOLO code runs inside your desktop shell with access to your files, accounts and network. Ward cannot contain it. Only use code you trust."
                color: Color.urgent
              }
              Label { visible: !!manager.inspected; text: manager.inspected ? manager.inspected.name + " · " + manager.inspected.id : ""; font.bold: true; width: parent.width }
              Label { visible: !!manager.inspected; text: manager.inspected ? manager.inspected.commit : ""; wrapMode: Text.WrapAnywhere; font.pixelSize: Style.font.bodySmall; color: Color.muted; width: parent.width }
              Toggle {
                objectName: "plugin-trust-confirm"
                visible: manager.yolo && !!manager.inspected
                width: parent.width
                label: "I trust this source to run unsandboxed"
                checked: manager.trustConfirmed
                onClicked: manager.trustConfirmed = !manager.trustConfirmed
              }
            }

            Column {
              visible: !manager.adding
              width: parent.width
              spacing: Style.spacing.labelGap
              Label { visible: manager.plugins.length === 0; text: "No installed plugins. Add one to get started."; width: parent.width; color: Color.muted }
              Repeater {
                model: manager.plugins
                delegate: Button {
                  required property var modelData
                  width: parent.width
                  implicitHeight: Math.max(64, label.implicitHeight + Style.spacing.rowGap * 2)
                  selected: manager.selectedId === modelData.id
                  focusable: true
                  enabled: !manager.busy
                  onClicked: manager.selectedId = modelData.id
                  Column {
                    id: label
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: Style.spacing.rowGap
                    spacing: Style.spacing.labelGap
                    Label { text: modelData.name || modelData.id; font.bold: true; width: parent.width }
                    Label { text: manager.modeLabel(modelData.executionMode) + " · " + (modelData.enabled ? "Enabled" : modelData.approved ? "Approved" : "Disabled"); color: Color.muted; width: parent.width; font.pixelSize: Style.font.bodySmall }
                  }
                }
              }
            }

            Label { visible: !manager.adding && !!manager.selected; text: manager.selected ? manager.selected.id : ""; color: Color.muted; width: parent.width }
            Label { visible: !manager.adding && !!manager.selected?.error; text: manager.selected?.error || ""; color: Color.urgent; width: parent.width }
            Label { visible: manager.confirmRemove; text: "Remove this plugin and delete its checkout? Ward approval will be revoked; saved plugin data is retained."; color: Color.urgent; width: parent.width }
            Label { visible: !!manager.error || !!manager.notice || manager.busy; text: manager.error || (manager.busy ? "Working: " + manager.operation + "…" : manager.notice); color: manager.error ? Color.urgent : Color.muted; width: parent.width }
          }
        }

        PanelSeparator { foreground: Color.popups.text; Layout.fillWidth: true }
        Flow {
          Layout.fillWidth: true
          Layout.preferredHeight: childrenRect.height
          spacing: Style.spacing.labelGap
          Button { text: manager.adding ? "Back" : "Add plugin"; focusable: true; implicitHeight: 40; enabled: !manager.busy; onClicked: { manager.adding = !manager.adding; manager.confirmRemove = false } }
          Button { visible: manager.adding; text: "Validate source"; objectName: "plugin-validate"; focusable: true; implicitHeight: 40; enabled: !manager.busy && !!manager.source.trim(); onClicked: manager.inspect() }
          Button { visible: manager.adding; text: manager.yolo ? "Add in YOLO mode" : "Add & review access"; objectName: "plugin-add"; selected: true; focusable: true; implicitHeight: 40; enabled: !manager.busy && !!manager.inspected && (!manager.yolo || manager.trustConfirmed); onClicked: manager.add() }
          Button { visible: !manager.adding && !!manager.selected?.sandboxed; text: "Review access"; focusable: true; implicitHeight: 40; enabled: !manager.busy; onClicked: root.review(manager.selectedId) }
          Button { visible: !manager.adding && !!manager.selected && !manager.selected.sandboxed && !manager.selected.enabled; text: "Enable plugin"; focusable: true; implicitHeight: 40; enabled: !manager.busy && !!manager.selected && !manager.selected.error; onClicked: manager.action("enable") }
          Button { visible: !manager.adding && !!manager.selected?.enabled; text: manager.selected?.sandboxed ? "Disable & revoke" : "Disable"; focusable: true; implicitHeight: 40; enabled: !manager.busy; onClicked: manager.action("disable") }
          Button { visible: !manager.adding && !!manager.selected; text: manager.confirmRemove ? "Confirm removal" : "Remove"; focusable: true; implicitHeight: 40; enabled: !manager.busy; onClicked: manager.action("remove") }
        }
      }
    }
  }
  component Label: Text {
    textFormat: Text.PlainText
    color: Color.popups.text
    font.family: Style.font.family
    font.pixelSize: Style.font.body
    wrapMode: Text.WordWrap
  }
}
