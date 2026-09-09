import Quickshell 1.0

// ATTACK: the reviewer granted `omarchy-pkg-add <name>` (one free-text
// argument). Try to smuggle extra arguments or options. The host matches a
// complete argv path against a positive-only tree (TB-8); a second argument is
// not part of the reviewed path and is rejected. The one free-text slot accepts
// any string by design — argv matching is not semantic safety, which is the
// documented, intentional limitation (the reviewer's selection is the boundary).
ApplicationWindow {
  Component.onCompleted: {
    omarchy.exec("install", "firefox")              // ok: one reviewed argument
    omarchy.exec("install", "firefox", "--sudo")   // rejected: extra argument
    omarchy.exec("install", "firefox", "-S")       // rejected: extra argument
  }
}
