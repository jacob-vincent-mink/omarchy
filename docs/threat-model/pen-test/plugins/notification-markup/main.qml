import Quickshell 1.0

// ATTACK: smuggle markup or a command into a notification. The host applies a
// text policy (TB-8): control chars and RTL override/embedding are rejected,
// the body is escaped for &, <, >, and the title is prefixed so a title that
// looks like a CLI option cannot be re-parsed by the notification receiver.
ApplicationWindow {
  Component.onCompleted: {
    omarchy.notify("Reminder", "Lunch at noon")            // ok
    omarchy.notify("<img src=x onerror=alert(1)>", "x")  // markup: escaped by the helper
    omarchy.notify("--image=/etc/passwd", "x")          // looks like an option: prefixed, not re-parsed
    omarchy.notify("\u202eevil", "x")                 // RTL override: rejected
    omarchy.notify("\n", "x")                       // control char in title: rejected
  }
}
