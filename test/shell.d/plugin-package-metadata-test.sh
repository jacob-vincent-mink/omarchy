#!/bin/bash

set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/base-test.sh"

runtime_root="$ROOT/native/plugin-runtime"
verifier="$runtime_root/packaging/arch/verify-archive-metadata.sh"
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
contract_path=usr/lib/omarchy/plugin-security/0.1.0/metadata/runtime-dependencies-v1.txt
contract="$scratch/payload/$contract_path"
pkginfo="$scratch/payload/.PKGINFO"
archive="$scratch/package.tar"
mkdir -p "$(dirname -- "$contract")"

# Only the metadata verifier is under test; this is not an installable payload.
reset_metadata() {
  cp "$runtime_root/packaging/runtime-dependencies-v1.txt" "$contract"
  {
    printf 'pkgname = omarchy-plugin-security\npkgver = 0.1.0-1\narch = x86_64\n'
    sed 's/^/depend = /' "$contract"
  } > "$pkginfo"
}

pack_metadata() {
  bsdtar -cf "$archive" -C "$scratch/payload" .PKGINFO "$contract_path"
}

expect_rejection() {
  local description=$1 expected=$2
  pack_metadata
  if "$verifier" "$archive" 0.1.0 > "$scratch/output" 2>&1; then
    fail "$description"
  fi
  grep -F "$expected" "$scratch/output" >/dev/null ||
    fail "$description" "rejected for an unexpected reason: $(<"$scratch/output")"
  pass "$description"
}

reset_metadata
pack_metadata
"$verifier" "$archive" 0.1.0 > "$scratch/output" 2>&1 ||
  fail "archive verifier accepts the committed dependency contract including SQLite" "$(<"$scratch/output")"
pass "archive verifier accepts the committed dependency contract including SQLite"

for mutation in missing duplicate constrained extra; do
  reset_metadata
  case "$mutation" in
    missing) sed -i '/^depend = sqlite$/d' "$pkginfo" ;;
    duplicate) printf 'depend = sqlite\n' >> "$pkginfo" ;;
    constrained) sed -i 's/^depend = sqlite$/depend = sqlite=0.0.0/' "$pkginfo" ;;
    extra) printf 'depend = unexpected-package\n' >> "$pkginfo" ;;
  esac
  expect_rejection "archive rejects $mutation package dependency metadata" \
    "archive dependencies differ from the configured runtime contract"
done

reset_metadata
sed -i '/^sqlite$/d' "$contract"
sed -i '/^depend = sqlite$/d' "$pkginfo"
expect_rejection "matching metadata cannot omit the required SQLite dependency" \
  "archive runtime contract differs from the required Arch package set"
