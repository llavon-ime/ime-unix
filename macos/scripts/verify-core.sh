#!/usr/bin/env bash
set -euo pipefail

# Compiles and runs the platform-independent Swift core against the engine.
# Requires swiftc (Linux or macOS), cmake, a C++ compiler and nlohmann-json.
# macos/scripts/verify-core-linux.sh wraps this in the Swift Docker image.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${LLAVON_IME_CORE_VERIFY_BUILD_DIR:-${ROOT_DIR}/build/verify-core}"

if [[ ! -f "${ROOT_DIR}/ime-core/table/bopomofo_char.json" ]]; then
    echo "ime-core is missing; run git submodule update --init ime-core" >&2
    exit 2
fi

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

configure_engine() {
    cmake -S "${ROOT_DIR}/engine" -B "${BUILD_DIR}/engine" \
        ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} \
        -DCMAKE_BUILD_TYPE=Release \
        "$@" \
        -DLLAVON_IME_ENGINE_BUILD_TESTS=OFF
}

if [[ "$(uname -s)" == "Darwin" ]]; then
    # On macOS the engine needs vcpkg for nlohmann-json.
    if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
        "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
    fi
    configure_engine -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    STANDARD_LIBRARY_FLAG="-lc++"
else
    configure_engine
    STANDARD_LIBRARY_FLAG="-lstdc++"
fi
cmake --build "${BUILD_DIR}/engine" --target llavon_ime_engine --parallel

swiftc -O -parse-as-library \
    -module-name LlavonIMECoreTests \
    -I "${ROOT_DIR}/engine/include" \
    "${ROOT_DIR}"/macos/Core/KeyTranslation.swift \
    "${ROOT_DIR}"/macos/Core/RenderSnapshot.swift \
    "${ROOT_DIR}"/macos/Core/CandidatePageWindow.swift \
    "${ROOT_DIR}"/macos/Core/ConfigJSON.swift \
    "${ROOT_DIR}"/macos/Core/EngineCore.swift \
    "${ROOT_DIR}"/macos/Tests/CoreTests.swift \
    "${BUILD_DIR}/engine/libllavon_ime_engine.a" \
    "${BUILD_DIR}/engine/protocol/libllavon_ime_protocol.a" \
    "${STANDARD_LIBRARY_FLAG}" \
    -o "${BUILD_DIR}/core-tests"

LLAVON_IME_TABLE_PATH="${LLAVON_IME_TABLE_PATH:-${ROOT_DIR}/ime-core/table/bopomofo_char.json}" \
    "${BUILD_DIR}/core-tests"
