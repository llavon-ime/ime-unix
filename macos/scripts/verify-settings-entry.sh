#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${LLAVON_IME_ENTRY_TEST_BUILD_DIR:-${ROOT_DIR}/build/settings-entry-tests}"
ENGINE_BUILD_DIR="${LLAVON_IME_ENTRY_ENGINE_BUILD_DIR:-${ROOT_DIR}/build/engine-tests}"
source "${ROOT_DIR}/macos/scripts/sparkle-config.sh"
if [[ ! -f "${SPARKLE_DIR}/Sparkle.framework/Sparkle" || ! -f "${ENGINE_BUILD_DIR}/libllavon_ime_engine.a" ]]; then
    echo 'Build the native input method/engine first (Sparkle and engine archive are required).' >&2
    exit 1
fi
mkdir -p "${BUILD_DIR}"
sources=()
for source in "${ROOT_DIR}"/macos/App/*.swift; do
    [[ "$(basename "$source")" == LlavonIMEApp.swift ]] || sources+=("$source")
done
swiftc -warnings-as-errors -parse-as-library -module-name SettingsEntryTests \
    -I "${ROOT_DIR}/engine/include" -F "${SPARKLE_DIR}" -framework Sparkle \
    -Xlinker -rpath -Xlinker "${SPARKLE_DIR}" \
    "${ROOT_DIR}"/macos/Core/*.swift "${sources[@]}" \
    "${ROOT_DIR}/macos/Tests/SettingsEntryTests.swift" \
    "${ENGINE_BUILD_DIR}/libllavon_ime_engine.a" -lc++ \
    "${ENGINE_BUILD_DIR}/protocol/libllavon_ime_protocol.a" \
    -framework Cocoa -framework InputMethodKit -framework Carbon \
    -o "${BUILD_DIR}/settings-entry-tests"
"${BUILD_DIR}/settings-entry-tests"
