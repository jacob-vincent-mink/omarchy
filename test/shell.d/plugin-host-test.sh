#!/bin/bash

set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/base-test.sh"

if "$ROOT/bin/omarchy-cmd-present" cargo; then
  cargo test --locked --manifest-path "$ROOT/native/plugin-host/Cargo.toml"
  pass "plugin host native tests"
else
  pass "cargo unavailable; skipping optional native plugin host tests"
fi
