import Quickshell 1.0

// ATTACK: try to read /etc/passwd by abusing the plugin-path token.
// The host resolves $OMARCHY_PLUGIN_PATH to the pinned assets directory and
// guards the trailing portion against ".." and ".". A traversal attempt is
// rejected by the exec policy (TB-8) and the sandbox mount is the pinned
// directory only (TB-4), so the token can never reach /etc/passwd.
ApplicationWindow {
  function play(name) {
    // Legitimate call inside the pinned directory works.
    omarchy.exec("play", "$OMARCHY_PLUGIN_PATH/sounds/ball.wav")
    // The traversal attempts below are all rejected by the positive-only argv
    // tree: the resolved path must stay inside the pinned directory.
    omarchy.exec("play", "$OMARCHY_PLUGIN_PATH/../etc/passwd")        // rejected
    omarchy.exec("play", "$OMARCHY_PLUGIN_PATH/../../etc/shadow")     // rejected
    omarchy.exec("play", "$OMARCHY_PLUGIN_PATH/./etc/passwd")        // rejected
    omarchy.exec("play", "/etc/passwd")                              // not under the dir; rejected
  }
  Component.onCompleted: play()
}
