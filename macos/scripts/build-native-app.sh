#!/usr/bin/env bash
set -euo pipefail
export COPYFILE_DISABLE=1

# Builds the native InputMethodKit frontend: engine static library, Swift app
# bundle, ad-hoc signature, optionally installing into the user's input
# methods. See macos/README.md for the manual test procedure.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP_NAME="LlavonIME"
BUNDLE_ID="${LLAVON_IME_BUNDLE_ID:-com.llavon.inputmethod.LlavonIME}"
VERSION="${LLAVON_IME_VERSION:-}"
BUILD_DIR="${LLAVON_IME_NATIVE_BUILD_DIR:-${ROOT_DIR}/build/native-macos}"
ENGINE_BUILD_DIR="${BUILD_DIR}/engine"
DIST_DIR="${LLAVON_IME_NATIVE_DIST_DIR:-${ROOT_DIR}/dist/macos}"
APP_DIR="${DIST_DIR}/${APP_NAME}.app"
INSTALL=0
INSTALL_USER=0
BUILD_SERVICE=1
SYSTEM_INSTALL_DIR="${LLAVON_IME_SYSTEM_INSTALL_DIR:-/Library/Input Methods}"
SYSTEM_PAYLOAD_DIR="${LLAVON_IME_SYSTEM_PAYLOAD_DIR:-/Library/Application Support/llavon-ime/payload}"
TIS_TOOL="${BUILD_DIR}/llavon-ime-tis"
UPDATES_ENABLED="${LLAVON_IME_ENABLE_UPDATES:-0}"
source "${ROOT_DIR}/macos/scripts/sparkle-config.sh"

for argument in "$@"; do
    case "${argument}" in
        --install) INSTALL=1 ;;
        # Install into the home directory instead of /Library. The package
        # installs system-wide, so this is only for machines without sudo.
        --user) INSTALL_USER=1 ;;
        # Skip the prediction service; the app alone is enough for UI work.
        --no-service) BUILD_SERVICE=0 ;;
        *)
            echo "Unknown argument: ${argument}" >&2
            echo "Usage: $0 [--install [--user] [--no-service]]" >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script only supports macOS." >&2
    exit 2
fi
for command in cmake swiftc codesign curl python3; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        if [[ "${command}" == "cmake" ]]; then
            echo "Install it with: brew install cmake" >&2
        fi
        exit 2
    fi
done
if ! command -v pkg-config >/dev/null 2>&1; then
    echo "Required command not found: pkg-config" >&2
    echo "Install it with: brew install pkg-config" >&2
    exit 2
fi

if [[ "${UPDATES_ENABLED}" != "0" && "${UPDATES_ENABLED}" != "1" ]]; then
    echo "LLAVON_IME_ENABLE_UPDATES must be 0 or 1." >&2
    exit 2
fi
if [[ "${UPDATES_ENABLED}" == "1" && -z "${LLAVON_IME_VERSION:-}" ]]; then
    echo "Enabled updates require an explicit release version." >&2
    exit 2
fi
prepare_sparkle

if [[ -z "${VERSION}" ]]; then
    # Prefer the highest release tag: the squashed main/preview branches do not
    # have the newest tags in their history, so git describe would report an
    # old version.
    VERSION="$(git -C "${ROOT_DIR}" tag --sort=-v:refname 2>/dev/null | head -1 | sed 's/^v//' || true)"
    if [[ -z "${VERSION}" ]]; then
        VERSION="$(git -C "${ROOT_DIR}" describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || true)"
    fi
fi
VERSION="${VERSION:-0.1.0}"

# The display version keeps a dev build traceable to its checkout, the way
# scripts/build-linux.sh does for the fcitx5 addon; release builds that pass
# LLAVON_IME_VERSION keep the clean version.
DISPLAY_VERSION="${VERSION}"
if [[ -z "${LLAVON_IME_VERSION:-}" ]]; then
    revision="$(git -C "${ROOT_DIR}" rev-list --count HEAD 2>/dev/null || true)"
    commit="$(git -C "${ROOT_DIR}" rev-parse --short=7 HEAD 2>/dev/null || true)"
    if [[ -n "${revision}" && -n "${commit}" ]]; then
        DISPLAY_VERSION="${VERSION}.r${revision}.g${commit}"
    fi
fi

if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "Initializing the vcpkg submodule..."
    git -C "${ROOT_DIR}" submodule update --init vcpkg
fi
if [[ ! -f "${ROOT_DIR}/ime-core/CMakeLists.txt" ]]; then
    echo "Initializing the ime-core submodule..."
    git -C "${ROOT_DIR}" submodule update --init ime-core
fi
if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    echo "Bootstrapping vcpkg..."
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

# Registers/enables the input source after the bundle is replaced. The package
# builds the same helper from packaging/macos/tools/tis.c.
build_tis_tool() {
    if [[ -x "${TIS_TOOL}" && "${TIS_TOOL}" -nt "${ROOT_DIR}/packaging/macos/tools/tis.c" ]]; then
        return 0
    fi
    xcrun clang -O2 -Wall -Wextra -Wconversion -Wsign-conversion -Werror -framework Carbon \
        -o "${TIS_TOOL}" "${ROOT_DIR}/packaging/macos/tools/tis.c"
}

echo "Building the engine..."
cmake -S "${ROOT_DIR}/engine" -B "${ENGINE_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DLLAVON_IME_ENGINE_BUILD_TESTS=OFF
cmake --build "${ENGINE_BUILD_DIR}" --target llavon_ime_engine --parallel

echo "Compiling the input method..."
rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/Contents/MacOS" "${APP_DIR}/Contents/Resources"
mkdir -p "${APP_DIR}/Contents/Resources/ThirdParty"
cp "${ROOT_DIR}/engine/data/README.md" "${APP_DIR}/Contents/Resources/ThirdParty/mixed-lexicon-attribution.md"
mkdir -p "${APP_DIR}/Contents/Frameworks"
ditto "${SPARKLE_DIR}/Sparkle.framework" "${APP_DIR}/Contents/Frameworks/Sparkle.framework"
cp "${SPARKLE_DIR}/LICENSE" "${APP_DIR}/Contents/Resources/ThirdParty/Sparkle-LICENSE"

swiftc -O -warnings-as-errors -parse-as-library \
    -target "$(uname -m)-apple-macos13.0" \
    -module-name LlavonIMEApp \
    -F "${SPARKLE_DIR}" -framework Sparkle \
    -Xlinker -rpath -Xlinker @executable_path/../Frameworks \
    -I "${ROOT_DIR}/engine/include" \
    "${ROOT_DIR}"/macos/Core/*.swift \
    "${ROOT_DIR}"/macos/App/*.swift \
    "${ENGINE_BUILD_DIR}/libllavon_ime_engine.a" \
    -lc++ \
    -framework Cocoa \
    -framework InputMethodKit \
    -framework Carbon \
    -o "${APP_DIR}/Contents/MacOS/${APP_NAME}"

sed -e "s/@APP_NAME@/${APP_NAME}/g" \
    -e "s/@BUNDLE_ID@/${BUNDLE_ID}/g" \
    -e "s/@VERSION@/${VERSION}/g" \
    -e "s/@DISPLAY_VERSION@/${DISPLAY_VERSION}/g" \
    "${ROOT_DIR}/macos/App/Info.plist.in" > "${APP_DIR}/Contents/Info.plist"
UPDATES_ENABLED="${UPDATES_ENABLED}" APP_PLIST="${APP_DIR}/Contents/Info.plist" python3 <<'PY'
import base64
import os
import plistlib
import re
from urllib.parse import urlparse

path = os.environ["APP_PLIST"]
with open(path, "rb") as file:
    plist = plistlib.load(file)
enabled = os.environ["UPDATES_ENABLED"] == "1"
plist["LlavonIMEUpdatesEnabled"] = enabled
if enabled:
    key = os.environ.get("SPARKLE_PUBLIC_ED_KEY", "").strip()
    if len(base64.b64decode(key, validate=True)) != 32:
        raise ValueError("SPARKLE_PUBLIC_ED_KEY must be an Ed25519 public key")
    version = plist["CFBundleVersion"]
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("updatable release version must use major.minor.patch")
    repository = os.environ.get("LLAVON_IME_RELEASE_REPOSITORY", "llavon-ime/ime-unix")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("invalid release repository")
    feed = os.environ.get("LLAVON_IME_UPDATE_FEED_URL", f"https://github.com/{repository}/releases/download/macos-updates/appcast.xml")
    if urlparse(feed).scheme != "https" or not urlparse(feed).netloc:
        raise ValueError("the update feed must use HTTPS")
    plist.update(SUFeedURL=feed, SUPublicEDKey=key, SUEnableAutomaticChecks=True,
                 SUAutomaticallyUpdate=True, SUScheduledCheckInterval=86400,
                 SUSendProfileInfo=False, SUVerifyUpdateBeforeExtraction=True,
                 SURequireSignedFeed=True)
with open(path, "wb") as file:
    plistlib.dump(plist, file)
PY
printf 'APPL????' > "${APP_DIR}/Contents/PkgInfo"
for icon in "${ROOT_DIR}"/macos/App/MenuIcon*.png; do
    [[ -f "${icon}" ]] || continue
    cp "${icon}" "${APP_DIR}/Contents/Resources/$(basename "${icon}")"
done

APP_ICON_SOURCE="${ROOT_DIR}/ime-unix-service/src/training/native/AppIcon.png"
if [[ -f "${APP_ICON_SOURCE}" ]] &&
   command -v sips >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
    iconset="$(mktemp -d)/AppIcon.iconset"
    mkdir -p "${iconset}"
    for size in 16 32 128 256 512; do
        sips -z "${size}" "${size}" "${APP_ICON_SOURCE}" \
            --out "${iconset}/icon_${size}x${size}.png" >/dev/null
        sips -z "$((size * 2))" "$((size * 2))" "${APP_ICON_SOURCE}" \
            --out "${iconset}/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns -o "${APP_DIR}/Contents/Resources/AppIcon.icns" "${iconset}"
    rm -rf "$(dirname "${iconset}")"
fi
for lproj in Base zh-Hant; do
    mkdir -p "${APP_DIR}/Contents/Resources/${lproj}.lproj"
    sed -e "s/@BUNDLE_ID@/${BUNDLE_ID}/g" \
        "${ROOT_DIR}/macos/App/InfoPlist.strings.in" \
        > "${APP_DIR}/Contents/Resources/${lproj}.lproj/InfoPlist.strings"
done

echo "Signing (ad-hoc)..."
sign_sparkle "${APP_DIR}/Contents/Frameworks/Sparkle.framework" -
codesign --force --sign - "${APP_DIR}"

if [[ "${INSTALL}" == "1" && "${BUILD_SERVICE}" == "1" ]]; then
    if [[ "${INSTALL_USER}" == "1" ]]; then
        service_prefix="${HOME}/Library/fcitx5"
    else
        # The package payload, which the app prefers over ~/Library/fcitx5.
        service_prefix="${SYSTEM_PAYLOAD_DIR}"
    fi
    echo "Building and installing the prediction service into ${service_prefix}..."
    LLAVON_IME_SERVICE_INSTALL_PREFIX="${service_prefix}" \
    LLAVON_IME_SERVICE_SKIP_NEXT_STEPS=1 \
        "${ROOT_DIR}/scripts/build-macos-service.sh"
    # A running service keeps its old binary; the engine spawns a new one on
    # the next prediction.
    pkill -x llavon-ime-unix-service 2>/dev/null || true
fi

if [[ "${INSTALL}" == "1" ]]; then
    if [[ "${INSTALL_USER}" == "1" ]]; then
        install_dir="${HOME}/Library/Input Methods"
        user_destination=""
    else
        # The package installs the app here. A second copy in the home
        # directory with the same bundle identifier shadows the system one
        # (and makes the package installer relocate its bundle), so drop that
        # copy first.
        install_dir="${SYSTEM_INSTALL_DIR}"
        user_destination="${HOME}/Library/Input Methods/${APP_NAME}.app"
    fi
    destination="${install_dir}/${APP_NAME}.app"

    # The helper re-registers the input source after the bundle moves.
    have_tis_tool=0
    if build_tis_tool 2>/dev/null; then
        have_tis_tool=1
    else
        echo "warning: could not build the input source helper; add 「拉風輸入法」 manually if it is missing." >&2
    fi

    if [[ "${INSTALL_USER}" == "1" ]]; then
        echo "Installing to ${destination}..."
    else
        echo "Installing to ${destination} (sudo required)..."
    fi
    if [[ "${have_tis_tool}" == "1" ]]; then
        source "${ROOT_DIR}/macos/scripts/input-source-lifecycle.sh"
        llavon_tis() { "${TIS_TOOL}" "$@"; }
        # Restore the user's original source also on a failed install.
        trap 'llavon_restore_input_source || true' EXIT
        llavon_prepare_input_source "${BUNDLE_ID}"
    fi
    pkill -x "${APP_NAME}" 2>/dev/null || true
    for _ in 1 2 3 4 5; do
        pgrep -x "${APP_NAME}" >/dev/null 2>&1 || break
        sleep 1
    done
    if pgrep -x "${APP_NAME}" >/dev/null 2>&1; then
        echo "The input method is still running; installation stopped before replacing the app." >&2
        exit 1
    fi
    if [[ "${INSTALL_USER}" == "1" ]]; then
        mkdir -p "${install_dir}"
        rm -rf "${destination}"
        ditto "${APP_DIR}" "${destination}"
        if [[ -d "${SYSTEM_INSTALL_DIR}/${APP_NAME}.app" ]]; then
            echo "warning: ${SYSTEM_INSTALL_DIR}/${APP_NAME}.app also exists and may shadow this copy." >&2
        fi
    else
        sudo mkdir -p "${install_dir}"
        sudo rm -rf "${destination}"
        sudo ditto "${APP_DIR}" "${destination}"
        # Keep the bundle readable for every user, like the package does.
        sudo chmod -R a+rX "${destination}"
        if [[ -n "${user_destination}" && -e "${user_destination}" ]]; then
            # Only the bundle that is left when the input source is registered
            # below is the one the text input system picks up.
            echo "Removing the user-level copy at ${user_destination}..."
            sudo rm -rf "${user_destination}"
        fi
    fi

    # Launch the input method once before touching the input source: a moved
    # bundle leaves the text input system pointing at the old path, and the
    # app's own registration is what makes the new bundle current. Launching
    # it here also makes the input method usable without logging out.
    if open "${destination}" 2>/dev/null; then
        for _ in 1 2 3 4 5; do
            pgrep -x "${APP_NAME}" >/dev/null 2>&1 && break
            sleep 1
        done
    else
        echo "note: could not launch 「拉風輸入法」 now; switch to it once and macOS will start it." >&2
    fi

    # Replacing (and moving) the bundle makes the text input system drop the
    # enabled input source, which is what leaves the input menu without
    # 拉風輸入法 after an install. Register the new bundle and, where the OS
    # allows it, enable the source again. TISEnableInputSource is a no-op for
    # third-party input methods, so `enable` writes the enabled-sources
    # preference directly on macOS 15 and earlier; the source then shows up at
    # the next login, when the input menu is rebuilt from it. macOS 26 keeps
    # the enabled third-party sources in a protected store that only System
    # Settings writes, so there the source has to be added there once. An
    # upgrade keeps the state it already has and needs nothing.
    if [[ "${have_tis_tool}" == "1" ]]; then
        "${TIS_TOOL}" register "${destination}" >/dev/null 2>&1 || true

        enable_output=""
        for _ in 1 2 3; do
            enable_output="$("${TIS_TOOL}" enable "${BUNDLE_ID}" 2>&1 || true)"
            if [[ "${enable_output}" == *"enabled=1"* || "${enable_output}" == *"deferred"* ]]; then
                break
            fi
            sleep 1
        done
        if [[ "${enable_output}" != *"enabled=1"* && "${enable_output}" != *"deferred"* ]]; then
            echo "warning: could not enable 「拉風輸入法」 (${enable_output}); add it under System Settings > Keyboard > Input Sources." >&2
        fi
    fi

    if [[ "${have_tis_tool}" == "1" ]]; then
        llavon_restore_input_source
        trap - EXIT
        "${TIS_TOOL}" status "${BUNDLE_ID}"
    fi

    cat <<'EOF'
Installed. Select 「拉風輸入法」 under System Settings > Keyboard > Input Sources.
The input source is re-registered on install; if it does not appear, log out
and back in once, then add it again.
EOF
fi

echo "Built ${APP_DIR}"
