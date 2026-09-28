#!/usr/bin/env bash
set -euo pipefail

# Builds and tests the native macOS input method, the prediction service and
# the pinned LoRA Trainer from this checkout, then installs them. This is the
# macOS counterpart of scripts/build-linux.sh; macos/README.md documents the
# manual steps and the test procedure.
#
# Usage:
#   scripts/build-macos.sh [--user] [--no-service]
#
#   --user        install into ~/Library instead of /Library (no sudo)
#   --no-service  build only the input method app (no prediction service and
#                 no LoRA Trainer)
#
# Environment overrides:
#   LLAVON_IME_MODEL_URL              model mirror
#   LLAVON_IME_DEBUG                  any non-empty value compiles in debug logging
#   LLAVON_IME_SKIP_LORA_TRAINER      skip the pinned LoRA Trainer install
#   LLAVON_IME_SKIP_LORA_GUI_RESTART  leave a running LoRA manager alone
#   LLAVON_IME_VERSION                app version override
#   LLAVON_IME_BUNDLE_ID              input method bundle id

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    cat <<'EOF'
Usage: scripts/build-macos.sh [--user] [--no-service]

  --user        install into ~/Library instead of /Library (no sudo)
  --no-service  build only the input method app (no prediction service and
                no LoRA Trainer)
EOF
}

for argument in "$@"; do
    case "${argument}" in
        --user | --no-service) ;;
        -h | --help) usage; exit 0 ;;
        *)
            echo "Unknown argument: ${argument}" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script only supports macOS." >&2
    exit 2
fi

case "$(uname -m)" in
    arm64) ;;
    *)
        echo "The macos presets currently only support Apple Silicon (arm64)." >&2
        exit 2
        ;;
esac

for command in git cmake curl pkg-config swiftc codesign; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        if [[ "${command}" == "cmake" || "${command}" == "pkg-config" ]]; then
            echo "Install it with: brew install ${command}" >&2
        fi
        exit 2
    fi
done
if ! xcode-select -p >/dev/null 2>&1; then
    echo "Xcode command line tools not found; run: xcode-select --install" >&2
    exit 2
fi

# The app and service scripts initialize what they need; doing it here fails
# fast and leaves one checkout ready for the whole build.
if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ||
      ! -f "${ROOT_DIR}/ime-core/CMakeLists.txt" ||
      ! -f "${ROOT_DIR}/lora-trainer/.git" ]]; then
    echo "Initializing git submodules..."
    git -C "${ROOT_DIR}" submodule update --init --recursive
fi
if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    echo "Bootstrapping vcpkg..."
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

echo "Building, testing, and installing the input method, the prediction service, and the LoRA Trainer..."
"${ROOT_DIR}/macos/scripts/build-native-app.sh" --install "$@"

cat <<'EOF'

macOS build, tests, and installation completed successfully.

Next steps:
  * Add 「拉風輸入法」 under System Settings > Keyboard > Input Sources if it is
    not in the input menu yet; the install re-registers the input source.
EOF
