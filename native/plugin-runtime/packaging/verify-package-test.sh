#!/bin/bash

set -euo pipefail

if (( $# != 3 )); then
  echo "Usage: $0 <staging-root> <runtime-version> <build-directory>" >&2
  exit 64
fi

source_root=$1
version=$2
build_directory=$3
if (( EUID != 0 )); then
  echo "package verifier negative tests require root to preserve and mutate package ownership" >&2
  exit 77
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
verifier=$script_dir/verify-package.sh
scratch=$(mktemp -d)
root=$scratch/root/usr/lib/omarchy/plugin-security/$version
cleanup() {
  rm -rf -- "$scratch"
}
trap cleanup EXIT

expect_failure() {
  local description=$1 expected=$2
  shift 2
  if "$@" >"$scratch/output" 2>&1; then
    echo "negative package test unexpectedly passed: $description" >&2
    exit 1
  fi
  grep -F "$expected" "$scratch/output" >/dev/null || {
    echo "negative package test failed for the wrong reason: $description" >&2
    cat "$scratch/output" >&2
    exit 1
  }
}

reset_fixture() {
  rm -rf -- "$scratch/root"
  cp -a "$source_root" "$scratch/root"
}

reject_mutation() {
  local description=$1 expected=$2
  shift 2
  reset_fixture
  "$@"
  expect_failure "$description" "$expected" "$verifier" --staging "$scratch/root" "$version"
}

expect_failure "non-canonical build prefix" "must be built for the canonical /usr prefix" \
  cmake -S "$script_dir/.." -B "$scratch/noncanonical-prefix" \
  -DCMAKE_INSTALL_PREFIX=/opt/omarchy -DBUILD_TESTING=OFF
expect_failure "non-canonical library root" "must use the canonical /usr/lib package root" \
  cmake -S "$script_dir/.." -B "$scratch/noncanonical-libdir" \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib64 -DBUILD_TESTING=OFF
expect_failure "install prefix override" "must be installed with its canonical /usr prefix" \
  cmake --install "$build_directory" --prefix "$scratch/prefix-bypass"
[[ ! -e $scratch/prefix-bypass ]] || {
  echo "non-canonical install wrote package members before rejecting its prefix" >&2
  exit 1
}

"$verifier" --staging "$source_root" "$version" >/dev/null

ln -s "$source_root" "$scratch/staging-alias"
expect_failure "aliased staging root" "staging root path is not canonical" \
  "$verifier" --staging "$scratch/staging-alias" "$version"

reject_mutation "unexpected file" "staging tree contains an unexpected member" \
  touch "$root/shell/unexpected.qml"

reject_mutation "unexpected directory" "staging tree contains an unexpected member" \
  mkdir "$root/unexpected-directory"

reject_mutation "unexpected symlink" "staging tree contains an unexpected member" \
  ln -s shell "$root/unexpected-link"

reject_mutation "unexpected special file" "staging tree contains an unexpected member" \
  mkfifo "$root/unexpected-fifo"

reset_fixture
mv "$scratch/root/usr/lib/omarchy" "$scratch/omarchy-real"
ln -s "$scratch/omarchy-real" "$scratch/root/usr/lib/omarchy"
expect_failure "aliased package ancestor" "staging tree member has wrong type: usr/lib/omarchy" "$verifier" --staging "$scratch/root" "$version"

reject_mutation "writable directory" "staging tree member has wrong mode" \
  chmod 775 "$root/shell"

reject_mutation "writable file" "staging tree member has wrong mode" \
  chmod 664 "$root/metadata/capability-catalog-v1.json"

reject_mutation "non-root owner" "staging tree member is not owned by root:root" \
  chown 1:0 "$root/metadata/capability-catalog-v1.json"

reject_mutation "non-root group" "staging tree member is not owned by root:root" \
  chown 0:1 "$root/metadata/capability-catalog-v1.json"

reject_mutation "multiply-linked package file" "staging tree file has multiple hard links" \
  ln "$root/metadata/capability-catalog-v1.json" "$scratch/policy-hardlink"

reject_mutation "missing package member" "staging tree omits required member" \
  rm "$root/metadata/capability-catalog-v1.json"

reset_fixture
capability_root=$root/capabilities.d
rm -f -- "$capability_root/"*.capability
rmdir "$capability_root"
[[ ! -e $capability_root ]] || {
  echo "missing capability root fixture still exists" >&2
  exit 1
}
expect_failure "missing mandatory capability root" "staging tree omits required member" "$verifier" --staging "$scratch/root" "$version"

reset_fixture
printf 'worker=/tmp/lib/omarchy/plugin-security/%s/bin/omarchy-plugin-qml-worker\n' "$version" >"$root/metadata/runtime-paths-v1.txt"
expect_failure "non-canonical runtime path contract" "runtime path contract is not canonical" "$verifier" --staging "$scratch/root" "$version"

reject_mutation "non-canonical worker path" "worker runtime path contract is not canonical" \
  perl -pi -e 's#/usr/lib/omarchy/plugin-security/#/tmp/lib/omarchy/plugin-security/#g' "$root/bin/omarchy-plugin-qml-worker"

reject_mutation "non-canonical bridge worker path" "bridge runtime path contract is not canonical" \
  perl -pi -e 's#/usr/lib/omarchy/plugin-security/#/tmp/lib/omarchy/plugin-security/#g' "$root/qml/Omarchy/PluginHost/libomarchy-plugin-host-bridge.so"

reject_mutation "version-pinned dependency" "runtime dependency contract differs from the required Arch package set" \
  sed -i 's/^qt6-base$/qt6-base=0.0.0-0/' "$root/metadata/runtime-dependencies-v1.txt"

reject_mutation "missing Omarchy dependency" "runtime dependency contract differs from the required Arch package set" \
  sed -i '/^omarchy$/d' "$root/metadata/runtime-dependencies-v1.txt"

reject_mutation "missing SQLite dependency" "runtime dependency contract differs from the required Arch package set" \
  sed -i '/^sqlite$/d' "$root/metadata/runtime-dependencies-v1.txt"

reset_fixture
printf '{\n' >"$root/metadata/capability-catalog-v1.json"
expect_failure "invalid catalog JSON" "command capability contract is unavailable" "$verifier" --staging "$scratch/root" "$version"

reject_mutation "retargeted command executor" "command provider profile does not pin the installed executor and capability contract" \
  sed -i 's#^executable=/usr/#executable=/tmp/#' "$root/providers.d/bash-execute.profile"

reject_mutation "retargeted desktop opener" "desktop-open provider profile does not pin the installed opener and capability contract" \
  sed -i 's#^executable=/usr/lib/omarchy/plugin-security/.*/bin/omarchy-plugin-desktop-opener#executable=/usr/bin/xdg-open#' "$root/providers.d/external-open-uri-https.profile"

reject_mutation "split network/media provider group" "network-fetch provider profile does not pin the installed provider and capability contract" \
  sed -i 's/^group=network\.media$/group=network.fetch/' "$root/providers.d/network-fetch.profile"

reject_mutation "retargeted media adapter" "media-stream provider profile does not pin the installed provider and capability contract" \
  sed -i 's/^adapter-class=activation-media-stream$/adapter-class=unbounded-media-stream/' "$root/providers.d/media-stream.profile"

reject_mutation "obsolete command policy argument" "command provider profile does not pin the installed executor and capability contract" \
  sed -i '$aarg=/etc/omarchy/plugin-command-profiles.d' "$root/providers.d/bash-execute.profile"

reset_fixture
sed -i 's/"manifestReferencesRequireExactPins": true/"manifestReferencesRequireExactPins": false/' \
  "$root/metadata/capability-catalog-v1.json"
expect_failure "invalid generated capability catalog" "generated capability catalog is invalid" "$verifier" --staging "$scratch/root" "$version"

reset_fixture
sed -i 's/^contract-digest=./contract-digest=f/' \
  "$root/capabilities.d/network.fetch.capability"
expect_failure "capability contract mismatch" "capability contract digest differs from catalog" "$verifier" --staging "$scratch/root" "$version"

reset_fixture
printf 'NOPE' | dd of="$root/bin/omarchy-plugin-qml-worker" bs=1 count=4 conv=notrunc status=none
expect_failure "invalid worker ELF" "omarchy-plugin-qml-worker is not an x86-64 PIE executable" "$verifier" --staging "$scratch/root" "$version"

reset_fixture
printf 'NOPE' | dd of="$root/qml/Omarchy/PluginHost/libomarchy-plugin-host-bridge.so" bs=1 count=4 conv=notrunc status=none
expect_failure "invalid bridge ELF" "libomarchy-plugin-host-bridge.so is not an x86-64 shared object" "$verifier" --staging "$scratch/root" "$version"

reset_fixture
printf '\n}\n' >>"$root/shell/SecureBarSurface.qml"
expect_failure "invalid installed shell QML" "installed shell QML syntax validation failed" "$verifier" --staging "$scratch/root" "$version"

reject_mutation "broken QML module import" "QML module import probe failed" \
  sed -i 's/^module .*/module Wrong.PluginHost/' "$root/qml/Omarchy/PluginHost/qmldir"

echo "secure plugin package verifier negative tests passed"
