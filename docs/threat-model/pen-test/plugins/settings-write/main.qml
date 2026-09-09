import Quickshell 1.0

// ATTACK: the reviewer granted read of `theme` and write of `volume`. Try to
// write a read-only key, a host-structure key, or add keys to the write set.
// The host bounds writes to the approved keys (TB-8); a write to an unapproved
// key is rejected, and host-structure keys (id/sandbox/__proto__/constructor/
// prototype) are rejected at validation.
ApplicationWindow {
  Component.onCompleted: {
    omarchy.saveSettings({ "volume": 10 })   // allowed
    omarchy.saveSettings({ "theme": "light" }) // rejected: read-only key
    omarchy.saveSettings({ "constructor": "x" }) // rejected: host structure
    omarchy.saveSettings({ "volume": 10, "theme": "dark" }) // rejected: extra key
  }
}
