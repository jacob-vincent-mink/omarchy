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
              text: "Select only the access you want to allow. Required requests must be granted before starting; all others are optional. Unselected access stays denied."
              width: parent.width
            }
            Toggle {
              width: parent.width
              visible: !!review.revision && review.revision.requests.network
              label: review.requestLabel("network", "Network")
              description: "Unrestricted Internet and local services. Selecting this clears scoped HTTP selections."
              checked: review.network
              onClicked: review.network = !review.network
            }
            Toggle {
              width: parent.width
              visible: !!review.revision && review.revision.requests.notifications
              label: review.requestLabel("notifications", "Notifications")
              objectName: "review-notifications"
              description: "Send bounded text notifications. No actions or images."
              checked: review.notifications
              onClicked: review.notifications = !review.notifications
            }
            Repeater {
              model: review.settingRequests
              delegate: Toggle {
                required property var modelData
                width: parent.width
                label: review.requestLabel("settings", (modelData.access === "read" ? "Read" : "Write") + " own setting: " + modelData.key)
                objectName: "review-setting-" + modelData.access + "-" + modelData.key
                description: modelData.access === "read" ? "Read this key and receive its updates. Other keys remain hidden."
                  : "Change this key only. Reading its value requires separate access."
                checked: review.settings[modelData.access].indexOf(modelData.key) !== -1
                onClicked: review.toggleSetting(modelData.access, modelData.key)
              }
            }
            Repeater {
              model: review.httpRequests
              delegate: Column {
                required property string modelData
                readonly property var ask: review.revision.requests.http[modelData]
                width: parent.width
                spacing: Style.spacing.labelGap
                Toggle {
                  width: parent.width
                  objectName: "review-http-" + modelData
                  label: "HTTP: " + modelData + (ask.required ? " · required" : "")
                  description: "Only the request scope below. Selecting this turns off unrestricted networking."
                  checked: review.http.indexOf(modelData) !== -1
                  onClicked: review.toggleHttp(modelData)
                }
                Label {
                  objectName: "review-http-scope-" + modelData
                  width: parent.width
                  wrapMode: Text.WrapAnywhere
                  text: ask.scope.method + " " + ask.scope.origin + ask.scope.path
                    + (ask.scope.subtree ? " (subtree)" : ask.scope.path.indexOf("*") !== -1 ? " (* matches one path segment)" : " (exact path)")
                    + "\nQuery: " + JSON.stringify(ask.scope.query)
                    + "\nJSON body: " + JSON.stringify(ask.scope.body)
                  font.pixelSize: Style.font.bodySmall
                  color: Color.muted
                }
              }
            }
            Toggle {
              width: parent.width
              visible: !!review.revision && review.revision.requests.openUrls
              label: review.requestLabel("openUrls", "Open web links")
              objectName: "review-open-urls"
              description: "Use your browser sessions, even without a click. Links can send data or reach local sites. HTTP(S) only."
              checked: review.openUrls
              onClicked: review.openUrls = !review.openUrls
            }
            Label {
              visible: review.execRequests.length > 0
              width: parent.width
              text: "Host commands use your existing accounts, files and network. Select each complete invocation below; additional arguments remain denied. Commands can have lasting effects, and revocation cannot undo them."
              color: Color.urgent
            }
            Repeater {
              model: review.execRequests
              delegate: Column {
                required property var modelData
                width: parent.width
                spacing: Style.spacing.labelGap
                Toggle {
                  width: parent.width
                  objectName: "review-exec-" + modelData.name + "-" + modelData.leaf
                  label: modelData.name + ": " + modelData.leaf + (modelData.required ? " · required" : "")
                  description: modelData.executable
                  checked: Object.prototype.hasOwnProperty.call(review.exec, modelData.name)
                    && review.exec[modelData.name].indexOf(modelData.leaf) !== -1
                  onClicked: review.toggleExec(modelData.name, modelData.leaf)
                }
                Label {
                  width: parent.width
                  wrapMode: Text.WrapAnywhere
                  text: "Arguments (one per line):\n" + modelData.command
                  font.pixelSize: Style.font.bodySmall
                  color: Color.muted
                }
              }
            }
            Column {
              width: parent.width
              visible: !!review.revision && review.revision.requests.media
              spacing: Style.spacing.labelGap
              Label { text: review.requestLabel("media", "One media player"); font.bold: true; width: parent.width }
              Label { text: "Read playback state and control this exact MPRIS service. Leave blank to deny."; color: Color.muted; width: parent.width }
              TextField {
                width: parent.width; implicitHeight: Math.max(40, Style.spacing.controlHeight)
                placeholderText: "org.mpris.MediaPlayer2.PlayerName"
                text: review.media
                onTextEdited: review.media = text
              }
            }
            Repeater {
              model: review.folderRequests
              Column {
                required property var modelData
                width: parent.width
                spacing: Style.spacing.labelGap
                Label {
                  text: "Folder: " + modelData.name + (modelData.required ? " · required" : "")
                  font.bold: true; width: parent.width
                }
                Label {
                  text: "Choose an absolute folder path, or leave blank to deny."
                    + (modelData.access === "readwrite" ? " This plugin requests read-write access." : " Files inside are readable, not writable.")
                  color: Color.muted; width: parent.width
                }
                TextField {
                  objectName: "review-folder-" + modelData.name
                  width: parent.width; implicitHeight: Math.max(40, Style.spacing.controlHeight)
                  placeholderText: "/absolute/folder"
                  text: typeof review.folders[modelData.name] === "string" ? review.folders[modelData.name] : ""
                  onTextEdited: review.setFolder(modelData.name, text)
                }
                Toggle {
                  objectName: "review-write-" + modelData.name
                  width: parent.width
                  visible: modelData.access === "readwrite"
                  label: "Allow changes to this folder"
                  description: "Read and change real host files. Disabling the plugin cannot undo completed writes."
                  checked: review.writableFolders[modelData.name] === true
                  onClicked: review.setWritable(modelData.name, !checked)
                }
              }
            }
            Toggle {
              visible: !!review.revision && review.revision.requests.storage
              objectName: "review-storage"
              width: parent.width
              label: review.requestLabel("storage", "Private persistent storage")
              description: "Keep this plugin's own data across restarts. Revocation removes access but retains saved data."
              checked: review.storage
              onClicked: review.storage = !review.storage
            }
            Label {
              visible: !!review.revision && !review.revision.requests.network && !review.revision.requests.notifications && review.settingRequests.length === 0 && !review.revision.requests.openUrls
                && !review.revision.requests.media && !review.revision.requests.storage && review.folderRequests.length === 0 && review.httpRequests.length === 0
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
