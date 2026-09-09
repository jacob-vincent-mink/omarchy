import Quickshell 1.0

// ATTACK: the reviewer approved api.example.test. Try other origins, schemes,
// ports, credentials, and paths. The host matches origin+method+path+query
// exactly (TB-8); the request vs scope check rejects any mismatch, and the
// resolved address is pinned to defeat DNS rebinding.
ApplicationWindow {
  Component.onCompleted: {
    // Approved.
    omarchy.http("actions", "https://api.example.test/groups/team/project/jobs?status=in_progress")
    // Every one of these is rejected by the exact scope match:
    omarchy.http("actions", "https://api.example.test.evil.test/groups/team/project/jobs?status=in_progress") // other host
    omarchy.http("actions", "https://api.example.test:444/groups/team/project/jobs?status=in_progress")        // other port
    omarchy.http("actions", "http://api.example.test/groups/team/project/jobs?status=in_progress")             // other scheme
    omarchy.http("actions", "https://user@api.example.test/groups/team/project/jobs")                        // credentials
    omarchy.http("actions", "https://api.example.test/groups/team/project/jobs?status=completed")            // query value
    omarchy.http("actions", "https://api.example.test/groups/team/project/jobs?status=in_progress&token=secret") // extra query
  }
}
