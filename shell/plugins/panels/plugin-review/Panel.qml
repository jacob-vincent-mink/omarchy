import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import Quickshell
import qs.Commons
import qs.Ui

Item {
  id: root
  property var shell: null
  property var manifest: null
  property bool opened: false
  readonly property alias review: review

  function open(payloadJson) {
    var payload = {}
    try { payload = JSON.parse(payloadJson || "{}") || {} } catch (e) {}
    opened = true
    if (!review.load(String(payload.id || "")) && review.busy)
      review.error = "Wait for the current command before reviewing another plugin."
  }
  function close() { opened = false }
  function dismiss() {
    if (shell && typeof shell.hide === "function") shell.hide("omarchy.plugin-review")
    else close()
  }

  Review { id: review }

  KeyboardPanel {
    id: panel
    objectName: "plugin-review-window"
    anchorItem: null
    bar: null
    owner: QtObject { function close() { root.dismiss() } }
    screen: Quickshell.screens[0] || null
    open: root.opened
    focusTarget: content
    contentWidth: fittedContentWidth(Style.space(540))
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
          Label { text: "Review plugin access"; font.pixelSize: Style.font.heading; font.bold: true; Layout.fillWidth: true }
          Button {
            objectName: "review-close"
            text: "Close"; focusable: true
            implicitHeight: Math.max(40, Style.spacing.controlHeight)
            onClicked: root.dismiss()
          }
        }

        ColumnLayout {
          Layout.fillWidth: true
          spacing: Style.spacing.labelGap
          Label { text: review.revision ? review.revision.name : review.pluginId; font.pixelSize: Style.font.title; font.bold: true; Layout.fillWidth: true }
          Label {
            text: review.revision ? review.pluginId + "  ·  " + review.revision.version : "Reading the installed plugin…"
            color: Color.muted; Layout.fillWidth: true
          }
          Label {
            text: review.current ? (review.current.enabled ? "Currently enabled" : review.current.approved ? "Currently approved, not enabled" : "Not approved") : "Checking current state…"
            color: Color.muted; Layout.fillWidth: true
          }
        }

        PanelSeparator { foreground: Color.popups.text; Layout.fillWidth: true }

        Controls.ScrollView {
          id: scroll
          objectName: "review-scroll"
          Layout.fillWidth: true
          Layout.fillHeight: true
          clip: true
          contentWidth: availableWidth
          Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
          Controls.ScrollBar.vertical.policy: contentHeight > availableHeight ? Controls.ScrollBar.AlwaysOn : Controls.ScrollBar.AlwaysOff

          Column {
            width: scroll.availableWidth
            spacing: Style.spacing.rowGap
            enabled: !!review.revision && !review.busy

            Label { text: "Exact revision"; font.bold: true; width: parent.width }
            Label {
              text: review.revision ? review.revision.revision : ""
              font.pixelSize: Style.font.bodySmall; color: Color.muted
              wrapMode: Text.WrapAnywhere; width: parent.width
            }
            Label {
              text: "Select only the access you want to allow. Unselected requests stay denied, even if an older revision was approved."
              width: parent.width
            }
            Toggle {
              width: parent.width
              visible: !!review.revision && review.revision.requests.network
              label: "Network"
              description: "Internet and local network services. The plugin can send data it can read."
              checked: review.network
              onClicked: review.network = !review.network
            }
            Toggle {
              width: parent.width
              visible: !!review.revision && review.revision.requests.notifications
              label: "Notifications"
              objectName: "review-notifications"
              description: "Send bounded text notifications. No actions or images."
              checked: review.notifications
              onClicked: review.notifications = !review.notifications
            }
            Toggle {
              width: parent.width
              visible: !!review.revision && review.revision.requests.settings
              label: "Save own settings"
              objectName: "review-settings"
              description: "Update this plugin's settings only. No configuration-file or other-plugin access."
              checked: review.settings
              onClicked: review.settings = !review.settings
            }
            Column {
              width: parent.width
              visible: !!review.revision && review.revision.requests.media
              spacing: Style.spacing.labelGap
              Label { text: "One media player"; font.bold: true; width: parent.width }
              Label { text: "Read playback state and control this exact MPRIS service. Leave blank to deny."; color: Color.muted; width: parent.width }
              TextField {
                width: parent.width; implicitHeight: Math.max(40, Style.spacing.controlHeight)
                placeholderText: "org.mpris.MediaPlayer2.PlayerName"
                text: review.media
                onTextEdited: review.media = text
              }
            }
            Repeater {
              model: review.revision ? review.revision.requests.read : []
              Column {
                required property string modelData
                width: parent.width
                spacing: Style.spacing.labelGap
                Label { text: "Read-only folder: " + modelData; font.bold: true; width: parent.width }
                Label { text: "Choose an absolute folder path, or leave blank to deny. Files inside are readable, not writable."; color: Color.muted; width: parent.width }
                TextField {
                  objectName: "review-folder-" + modelData
                  width: parent.width; implicitHeight: Math.max(40, Style.spacing.controlHeight)
                  placeholderText: "/absolute/folder"
                  text: review.folders[modelData] || ""
                  onTextEdited: review.setFolder(modelData, text)
                }
              }
            }
            Label {
              visible: !!review.revision && review.revision.requests.storage
              text: "Private storage is unavailable in this preview and will not be granted. This plugin may need it to work."
              color: Color.urgent; width: parent.width
            }
            Label {
              visible: !!review.revision && !review.revision.requests.network && !review.revision.requests.notifications && !review.revision.requests.settings
                && !review.revision.requests.media && !review.revision.requests.storage && review.revision.requests.read.length === 0
              text: "This revision requests no additional access."
              color: Color.muted; width: parent.width
            }
          }
        }

        Label {
          Layout.fillWidth: true
          text: review.error || (review.busy ? "Working: " + review.operation + "…"
            : review.current && review.current.enabled && !review.selectionApproved ? "Disable and revoke before changing a running plugin's access." : review.notice)
          color: review.error ? Color.urgent : Color.muted
          font.pixelSize: Style.font.bodySmall
          maximumLineCount: 4
          elide: Text.ElideRight
          visible: text !== ""
        }
        Flow {
          Layout.fillWidth: true
          spacing: Style.spacing.controlGap
          Button {
            text: "Refresh"; focusable: true; enabled: !review.busy
            implicitHeight: Math.max(40, Style.spacing.controlHeight)
            onClicked: review.load(review.pluginId)
          }
          Button {
            text: "Disable & revoke"; focusable: true; enabled: !review.busy && !!review.revision
            objectName: "review-revoke"
            implicitHeight: Math.max(40, Style.spacing.controlHeight)
            onClicked: review.revoke()
          }
          Button {
            text: review.current && review.current.enabled ? "Enabled" : review.selectionApproved ? "Enable plugin" : "Approve selection"
            objectName: "review-approve"
            selected: true; focusable: true; enabled: !review.busy && !!review.revision && !(review.current && review.current.enabled)
            implicitHeight: Math.max(40, Style.spacing.controlHeight)
            onClicked: review.selectionApproved ? review.enable() : review.approve()
          }
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
