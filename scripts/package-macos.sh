#!/usr/bin/env bash
set -euo pipefail
export COPYFILE_DISABLE=1

# Builds the native macOS input method package: LlavonIME.app from macos/,
# the optional AI prediction service from ime-unix-service (Metal, arm64 only)
# and its tables, plus the bundled GGUF model. Output:
#   dist/macos/llavon-ime-<version>-arm64.pkg
#
# Signing is applied when the matching identities are provided:
#   DEVELOPER_ID_APPLICATION  signs the app and the service
#   DEVELOPER_ID_INSTALLER    signs the installer package
# Without them the app keeps its ad-hoc signature and the pkg is unsigned.
#
# Environment overrides:
#   LLAVON_IME_VERSION            package version (default 0.2.1)
#   LLAVON_IME_BUNDLE_ID          input method bundle id
#   LLAVON_IME_PACKAGE_MODEL_PATH .gguf bundled as the default model
#   LLAVON_IME_VCPKG_FEATURES     service vcpkg features (default llama-metal)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/macos/scripts/sparkle-config.sh"
APP_NAME="LlavonIME"
BUNDLE_ID="${LLAVON_IME_BUNDLE_ID:-com.llavon.inputmethod.LlavonIME}"
VERSION="${LLAVON_IME_VERSION:-0.2.1}"
ARCH="$(uname -m)"
DIST_DIR="${LLAVON_IME_DIST_DIR:-${ROOT_DIR}/dist/macos}"
PKGROOT="${DIST_DIR}/pkgroot"
PKG_IDENTIFIER="${LLAVON_IME_PKG_IDENTIFIER:-llavon-ime}"
PAYLOAD_PREFIX="${LLAVON_IME_MACOS_PAYLOAD_PREFIX:-/Library/Application Support/llavon-ime/payload}"
MODEL_INSTALL_DIR="/Library/Application Support/llavon-ime/models"
MODEL_PATH="${LLAVON_IME_PACKAGE_MODEL_PATH:-}"
VCPKG_FEATURES="${LLAVON_IME_VCPKG_FEATURES:-llama-metal}"
SERVICE_BUILD_DIR="${LLAVON_IME_UNIX_SERVICE_BUILD_DIR:-${ROOT_DIR}/build/package-llavon-ime-unix-service-macos-${ARCH}}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script only supports macOS." >&2
    exit 2
fi

if [[ "${ARCH}" != "arm64" ]]; then
    echo "The macOS package currently bundles the arm64 service; found ${ARCH}." >&2
    exit 2
fi

for command in cmake swiftc codesign pkgbuild productbuild pkg-config xcrun; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        if [[ "${command}" == "pkg-config" ]]; then
            echo "Install it with: brew install pkg-config" >&2
        fi
        exit 2
    fi
done

if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "Initializing the vcpkg submodule..."
    git -C "${ROOT_DIR}" submodule update --init vcpkg
fi
if [[ ! -f "${ROOT_DIR}/ime-core/CMakeLists.txt" ]]; then
    echo "Initializing the ime-core submodule..."
    git -C "${ROOT_DIR}" submodule update --init ime-core
fi
if [[ ! -f "${ROOT_DIR}/lora-trainer/.git" ]]; then
    echo "Initializing the lora-trainer submodule..."
    git -C "${ROOT_DIR}" submodule update --init lora-trainer
fi
if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "vcpkg was not found; run: git -C \"${ROOT_DIR}\" submodule update --init vcpkg" >&2
    exit 2
fi
if [[ "$(git -C "${ROOT_DIR}/vcpkg" rev-parse --is-shallow-repository)" == "true" ]]; then
    git -C "${ROOT_DIR}/vcpkg" fetch --unshallow
fi
if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    echo "Bootstrapping vcpkg..."
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

if [[ -z "${MODEL_PATH}" ]]; then
    shopt -s nullglob
    model_candidates=("${ROOT_DIR}"/models/*.gguf)
    shopt -u nullglob
    if [[ ${#model_candidates[@]} -eq 1 ]]; then
        MODEL_PATH="${model_candidates[0]}"
    elif [[ ${#model_candidates[@]} -eq 0 ]]; then
        cat >&2 <<'EOF'
No .gguf model was found under models/.

Set LLAVON_IME_PACKAGE_MODEL_PATH=/path/to/model.gguf to build the installer with a bundled model.
EOF
        exit 2
    else
        cat >&2 <<'EOF'
Multiple .gguf models were found under models/.

Set LLAVON_IME_PACKAGE_MODEL_PATH=/path/to/model.gguf to choose the model bundled in the installer.
EOF
        exit 2
    fi
fi

if [[ ! -f "${MODEL_PATH}" || "${MODEL_PATH}" != *.gguf ]]; then
    echo "LLAVON_IME_PACKAGE_MODEL_PATH must point to a .gguf file: ${MODEL_PATH}" >&2
    exit 2
fi

MODEL_INSTALL_PATH="${MODEL_INSTALL_DIR}/$(basename "${MODEL_PATH}")"

rm -rf "${PKGROOT}"
mkdir -p "${PKGROOT}" "${DIST_DIR}"

echo "Building the native input method app..."
LLAVON_IME_VERSION="${VERSION}" \
LLAVON_IME_BUNDLE_ID="${BUNDLE_ID}" \
    "${ROOT_DIR}/macos/scripts/build-native-app.sh"

app_source="${ROOT_DIR}/dist/macos/${APP_NAME}.app"
if [[ ! -x "${app_source}/Contents/MacOS/${APP_NAME}" ]]; then
    echo "Built app not found: ${app_source}" >&2
    exit 1
fi

echo "Building and testing ime-unix-service (Metal; the first build can take a while)..."
unix_service_cmake_args=(
    -S "${ROOT_DIR}/ime-unix-service"
    -B "${SERVICE_BUILD_DIR}"
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    -DCMAKE_INSTALL_PREFIX="${PAYLOAD_PREFIX}"
    -DLLAVON_IME_INSTALLED_LORA_TRAINER_PATH="/Library/Application Support/llavon-ime/tools/lora/llavon-lora"
    -DLLAVON_IME_INSTALLED_MODEL_PATH="${MODEL_INSTALL_PATH}"
    -DIME_UNIX_SERVICE_BUILD_TESTS=ON
)
if [[ -n "${VCPKG_FEATURES}" ]]; then
    unix_service_cmake_args+=(-DVCPKG_MANIFEST_FEATURES="${VCPKG_FEATURES}")
fi
cmake "${unix_service_cmake_args[@]}"
cmake --build "${SERVICE_BUILD_DIR}"
ctest --test-dir "${SERVICE_BUILD_DIR}" --output-on-failure
DESTDIR="${PKGROOT}" cmake --install "${SERVICE_BUILD_DIR}"

app_root="${PKGROOT}/Library/Input Methods"
mkdir -p "${app_root}"
/usr/bin/ditto --norsrc --noextattr "${app_source}" "${app_root}/${APP_NAME}.app"

payload_root="${PKGROOT}${PAYLOAD_PREFIX}"
model_root="${PKGROOT}${MODEL_INSTALL_DIR}"
mkdir -p "${model_root}"
install -m 0644 "${MODEL_PATH}" "${model_root}/$(basename "${MODEL_PATH}")"

license_root="${payload_root}/share/llavon-ime/licenses"
cmake \
    -DVCPKG_INSTALLED_DIR="${SERVICE_BUILD_DIR}/vcpkg_installed" \
    -DDESTINATION="${license_root}" \
    -DPROJECT_ROOT="${ROOT_DIR}" \
    -P "${ROOT_DIR}/scripts/install-licenses.cmake"

echo "Building the TIS registration helper..."
tool_root="${PKGROOT}/Library/Application Support/llavon-ime/tools"
mkdir -p "${tool_root}"
xcrun clang -O2 -Wall -Wextra -framework Carbon \
    -o "${tool_root}/llavon-ime-tis" \
    "${ROOT_DIR}/packaging/macos/tools/tis.c"

echo "Bundling the pinned LoRA Trainer release..."
trainer_staging="$(mktemp -d "${TMPDIR:-/tmp}/llavon-lora-package.XXXXXX")"
"${SERVICE_BUILD_DIR}/llavon-ime-lora" install-trainer --output-dir "${trainer_staging}"
mkdir -p "${tool_root}/lora"
cp -a "${trainer_staging}/." "${tool_root}/lora/"
chmod -R a+rX "${tool_root}/lora"
chmod 0644 "${tool_root}/lora/trainer-release.json"
rm -rf "${trainer_staging}"

# The bundled LoRA trainer ships its own license in the release archive.
if [[ -f "${tool_root}/lora/LICENSE" ]]; then
    install -d "${license_root}/llavon-lora-trainer"
    install -m 0644 "${tool_root}/lora/LICENSE" "${license_root}/llavon-lora-trainer/LICENSE"
fi

# The uninstaller cleans the files the cask's uninstall does not know about
# (the legacy fcitx5 payload) and asks before removing a leftover Fcitx5.app.
install -m 0755 "${ROOT_DIR}/packaging/macos/uninstall.sh" \
    "${PKGROOT}/Library/Application Support/llavon-ime/uninstall.sh"

if command -v xattr >/dev/null 2>&1; then
    xattr -cr "${PKGROOT}" || true
fi
find "${PKGROOT}" -name '._*' -delete

# The bundled LoRA Trainer keeps its upstream signature: re-signing its
# apphost with a hardening runtime would reject the native libraries that ship
# with it.
if [[ -n "${DEVELOPER_ID_APPLICATION:-}" ]]; then
    echo "Signing with Developer ID Application: ${DEVELOPER_ID_APPLICATION}"
    sign_sparkle "${app_root}/${APP_NAME}.app/Contents/Frameworks/Sparkle.framework" "${DEVELOPER_ID_APPLICATION}"
    codesign --force --timestamp --options runtime \
        --sign "${DEVELOPER_ID_APPLICATION}" \
        "${app_root}/${APP_NAME}.app"
    codesign --force --timestamp --options runtime \
        --sign "${DEVELOPER_ID_APPLICATION}" \
        "${payload_root}/bin/llavon-ime-unix-service"
    codesign --force --timestamp --options runtime \
        --sign "${DEVELOPER_ID_APPLICATION}" \
        "${payload_root}/bin/llavon-ime-lora"
    codesign --force --deep --timestamp --options runtime \
        --sign "${DEVELOPER_ID_APPLICATION}" \
        "${payload_root}/bin/llavon-ime-lora-gui.app"
    codesign --force --timestamp --options runtime \
        --sign "${DEVELOPER_ID_APPLICATION}" \
        "${payload_root}/bin/llavon-ime-lora-backend"
else
    echo "No Developer ID Application identity; keeping the ad-hoc app signature."
    codesign --force --timestamp=none --sign - \
        "${payload_root}/bin/llavon-ime-unix-service"
    codesign --force --timestamp=none --sign - \
        "${payload_root}/bin/llavon-ime-lora"
    codesign --force --deep --timestamp=none --sign - \
        "${payload_root}/bin/llavon-ime-lora-gui.app"
    codesign --force --timestamp=none --sign - \
        "${payload_root}/bin/llavon-ime-lora-backend"
fi

required_files=(
    "${app_root}/${APP_NAME}.app/Contents/MacOS/${APP_NAME}"
    "${app_root}/${APP_NAME}.app/Contents/Info.plist"
    "${payload_root}/bin/llavon-ime-unix-service"
    "${payload_root}/bin/llavon-ime-lora"
    "${payload_root}/bin/llavon-ime-lora-gui"
    "${payload_root}/bin/llavon-ime-lora-backend"
    "${payload_root}/bin/llavon-ime-lora-gui.app/Contents/Info.plist"
    "${payload_root}/share/llavon-ime/tables/bopomofo_char.json"
    "${payload_root}/share/llavon-ime/tables/tokens/bpmf.json"
    "${payload_root}/share/llavon-ime/tables/tokens/chars.json"
    "${payload_root}/share/llavon-ime/tables/tokens/latin.json"
    "${payload_root}/share/llavon-ime/tables/tokens/special_tokens.json"
    "${license_root}/llavon-ime/LICENSE"
    "${license_root}/ime-unix-service/LICENSE"
    "${license_root}/ime-core/LICENSE"
    "${license_root}/nlohmann-json/LICENSE"
    "${license_root}/llama-cpp/LICENSE"
    "${license_root}/llavon-ime-model/NOTICE"
    "${license_root}/mcbopomofo-symbols/NOTICE"
    "${PKGROOT}${MODEL_INSTALL_PATH}"
    "${tool_root}/llavon-ime-tis"
    "${tool_root}/lora/llavon-lora"
    "${tool_root}/lora/trainer-release.json"
    "${PKGROOT}/Library/Application Support/llavon-ime/uninstall.sh"
)

for file in "${required_files[@]}"; do
    if [[ ! -f "${file}" ]]; then
        echo "Missing package payload file: ${file}" >&2
        exit 1
    fi
done

if [[ -e "${PKGROOT}/Users" ]]; then
    echo "Package root unexpectedly contains /Users; install paths are not relocatable." >&2
    exit 1
fi

unsigned_pkg="${DIST_DIR}/llavon-ime-${VERSION}-${ARCH}.pkg"
# The component plist turns bundle relocation off: with it, Installer always
# writes /Library/Input Methods/LlavonIME.app instead of upgrading a same-id
# copy in the home directory.
pkgbuild \
    --root "${PKGROOT}" \
    --scripts "${ROOT_DIR}/packaging/macos/scripts" \
    --component-plist "${ROOT_DIR}/packaging/macos/LlavonIME-component.plist" \
    --identifier "${PKG_IDENTIFIER}" \
    --version "${VERSION}" \
    --install-location / \
    "${unsigned_pkg}"

if [[ -n "${DEVELOPER_ID_INSTALLER:-}" ]]; then
    signed_pkg="${DIST_DIR}/llavon-ime-${VERSION}-${ARCH}-signed.pkg"
    productsign --sign "${DEVELOPER_ID_INSTALLER}" "${unsigned_pkg}" "${signed_pkg}"
    echo "Built signed package: ${signed_pkg}"
else
    echo "Built package: ${unsigned_pkg}"
fi
echo "Bundled model: ${MODEL_INSTALL_PATH}"
