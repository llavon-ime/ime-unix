#!/usr/bin/env bash
set -euo pipefail

# Builds and runs the standard test suites:
#   - llavon_ime_tests       engine unit tests (internals that keys cannot reach)
#   - llavon_ime_rawkey_tests the raw-key behaviour suite every frontend shares
#                             (engine/tests/rawkey, including a service-backed
#                             prediction test)
# Host-free, so Linux and macOS run exactly the same suites.
# LLAVON_IME_WARNINGS_AS_ERRORS=ON makes compiler warnings fail the build (CI).

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${LLAVON_IME_ENGINE_TEST_BUILD_DIR:-${ROOT_DIR}/build/engine-tests}"

if [[ ! -f "${ROOT_DIR}/ime-core/CMakeLists.txt" ]]; then
    git -C "${ROOT_DIR}" submodule update --init ime-core
fi

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

TOOLCHAIN_ARGS=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
        echo "Initializing the vcpkg submodule..."
        git -C "${ROOT_DIR}" submodule update --init vcpkg
    fi
    if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
        echo "Bootstrapping vcpkg..."
        "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
    fi
    TOOLCHAIN_ARGS=(-DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
fi

cmake -S "${ROOT_DIR}/engine" -B "${BUILD_DIR}" \
    ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} \
    ${TOOLCHAIN_ARGS[@]+"${TOOLCHAIN_ARGS[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLAVON_IME_WARNINGS_AS_ERRORS="${LLAVON_IME_WARNINGS_AS_ERRORS:-OFF}" \
    -DLLAVON_IME_ENGINE_BUILD_TESTS=ON
cmake --build "${BUILD_DIR}" --target llavon_ime_tests llavon_ime_rawkey_tests --parallel

# The suites read the shared table through the compiled-in test path.
cd "${ROOT_DIR}"
"${BUILD_DIR}/tests/llavon_ime_tests"
"${BUILD_DIR}/tests/llavon_ime_rawkey_tests"
