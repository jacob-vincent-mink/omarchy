import QtQuick

QtObject {
  id: root

  property string directResult: "testing"
  property string traversalResult: "testing"
  readonly property bool complete: directResult !== "testing"
    && traversalResult !== "testing"
  readonly property bool blocked: complete && directResult === "blocked"
    && traversalResult === "blocked"
  readonly property string verdict: !complete ? "TESTING"
    : blocked ? "BLOCKED" : "BREACH"

  function run() {
    var packaged = runtime.readPackagedText("../../../../etc/passwd", 4096)
    traversalResult = packaged && packaged.length > 0 ? "read " + packaged.length + " bytes" : "blocked"

    try {
      var request = new XMLHttpRequest()
      request.onreadystatechange = function() {
        if (request.readyState !== XMLHttpRequest.DONE) return
        directTimeout.stop()
        directResult = request.responseText && request.responseText.length > 0
          ? "read " + request.responseText.length + " bytes" : "blocked"
      }
      request.onerror = function() {
        directTimeout.stop()
        directResult = "blocked"
      }
      request.open("GET", "file:///etc/passwd")
      request.send()
    } catch (error) {
      directTimeout.stop()
      directResult = "blocked"
    }
  }

  property Timer directTimeout: Timer {
    interval: 1500
    onTriggered: root.directResult = "blocked"
  }

  Component.onCompleted: {
    directTimeout.start()
    run()
  }
}
