#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build/settings-host-tests"
mkdir -p "$BUILD"
swiftc -warnings-as-errors -parse-as-library "$ROOT/macos/Core/SettingsHost.swift" \
    "$ROOT/macos/Tests/SettingsHostTests.swift" -o "$BUILD/settings-host-tests"
export LLAVON_IME_SETTINGS_PORT="org.llavon-ime.settings.test.$RANDOM.$$"
export LLAVON_TEST_HOST_HELPER="${LLAVON_TEST_HOST_HELPER:?pass the built native GUI executable}"
"$BUILD/settings-host-tests"
