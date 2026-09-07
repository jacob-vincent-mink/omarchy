import QtQuick

QtObject {
  id: root

  property var activeCalls: []

  function decodeText(value) {
    try {
      var result = JSON.parse(value)
      return result.found === true && typeof result.value === "string" ? result.value : null
    } catch (_) { return null }
  }

  function watch(call, success, failure) {
    if (!call) {
      failure("request-rejected")
      return
    }
    activeCalls = activeCalls.concat([call])
    var done = function() {
      if (!call.finished) return
      try { call.finishedChanged.disconnect(done) } catch (_) {}
      root.activeCalls = root.activeCalls.filter(function(item) { return item !== call })
      if (call.ok) success(call.utf8Text)
      else failure(String(call.error || "request-failed"))
    }
    if (call.finished) done()
    else call.finishedChanged.connect(done)
  }

  function readText(key, success, failure) {
    watch(runtime.invoke("storage.private", "read", {
      key: key
    }), function(value) { success(decodeText(value)) }, failure)
  }

  function writeText(key, value, success, failure) {
    watch(runtime.invoke("storage.private", "write", {
      key: key, value: String(value)
    }), success, failure)
  }
}
