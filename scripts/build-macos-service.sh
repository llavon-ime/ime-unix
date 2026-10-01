#!/usr/bin/env bash
set -euo pipefail

# Builds and tests the AI prediction service (ime-unix-service) from this
# checkout and installs it where the native macOS input method looks for it.
# The model is installed at the package default location and reused when
# present, and the pinned LoRA Trainer is installed next to the service
# payload, matching the package layout.
#
# The input method frontend itself is built by macos/scripts/build-native-app.sh,
# which calls this script with LLAVON_IME_SERVICE_INSTALL_PREFIX when it
# installs the app. It works without this service (table candidates are the
# fallback, predictions are additive), so only run this when you want the
# llama service.
#
# Currently Apple Silicon only: the macos preset uses the arm64-osx-llavon
# triplet with the Metal backend.
#
# Environment overrides:
#   LLAVON_IME_MODEL_URL                model mirror
#   LLAVON_IME_DEBUG                    any non-empty value compiles in debug logging
#   LLAVON_IME_SERVICE_INSTALL_PREFIX   install prefix (default ~/Library/fcitx5;
#                                       the package payload path installs with sudo)
#   LLAVON_IME_SERVICE_SKIP_NEXT_STEPS  set to 1 to drop the closing hints
#   LLAVON_IME_LORA_TRAINER_DIR         LoRA Trainer directory (defaults to
#                                       <payload>/tools/lora for the package
#                                       payload, <prefix>/lib/llavon-ime/tools/lora
#                                       otherwise)
#   LLAVON_IME_SKIP_LORA_TRAINER        skip the pinned LoRA Trainer install
#   LLAVON_IME_SKIP_LORA_GUI_RESTART    leave a running LoRA manager alone

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL_FILE="llavon-ime-llama-250m-Q4_K_M.gguf"
MODEL_URL="${LLAVON_IME_MODEL_URL:-https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/resolve/main/${MODEL_FILE}}"
MODEL_INSTALL_PATH="/Library/Application Support/llavon-ime/models/${MODEL_FILE}"
INSTALL_PREFIX="${LLAVON_IME_SERVICE_INSTALL_PREFIX:-${HOME}/Library/fcitx5}"

# The package puts the pinned trainer next to the payload under
# /Library/Application Support/llavon-ime/tools/lora; a user install keeps it
# inside the install prefix, where the compiled-in path looks for it.
SYSTEM_PAYLOAD_PREFIX="/Library/Application Support/llavon-ime/payload"
if [[ -n "${LLAVON_IME_LORA_TRAINER_DIR:-}" ]]; then
    LORA_TRAINER_DIR="${LLAVON_IME_LORA_TRAINER_DIR}"
elif [[ "${INSTALL_PREFIX}" == "${SYSTEM_PAYLOAD_PREFIX}" ]]; then
    LORA_TRAINER_DIR="/Library/Application Support/llavon-ime/tools/lora"
else
    LORA_TRAINER_DIR="${INSTALL_PREFIX}/lib/llavon-ime/tools/lora"
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script only supports macOS." >&2
    exit 2
fi

case "$(uname -m)" in
    arm64) ;;
    *)
        echo "The macos preset currently only supports Apple Silicon (arm64)." >&2
        exit 2
        ;;
esac

for command in git cmake curl install sed pkg-config unzip; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        if [[ "${command}" == "pkg-config" ]]; then
            echo "Install it with: brew install pkg-config" >&2
        fi
        exit 2
    fi
done

if ! xcode-select -p >/dev/null 2>&1; then
    echo "Xcode command line tools not found; run: xcode-select --install" >&2
    exit 2
fi
if ! command -v clang++ >/dev/null 2>&1; then
    echo "Required compiler not found: clang++" >&2
    exit 2
fi
if ! command -v ninja >/dev/null 2>&1; then
    echo "Note: ninja not found; CMake will fall back to Unix Makefiles." >&2
fi

if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ||
      ! -f "${ROOT_DIR}/ime-core/CMakeLists.txt" ||
      ! -f "${ROOT_DIR}/lora-trainer/.git" ]]; then
    echo "Initializing git submodules..."
    git -C "${ROOT_DIR}" submodule update --init --recursive
fi
if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "vcpkg was not found; run: git -C \"${ROOT_DIR}\" submodule update --init vcpkg" >&2
    exit 2
fi

if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    echo "Bootstrapping vcpkg..."
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

# The release package installs the model globally; detect it directly there
# and only download when it is missing.
SUDO=()
if ((EUID != 0)); then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "Required command not found: sudo" >&2
        exit 2
    fi
    SUDO=(sudo)
fi

if [[ -s "${MODEL_INSTALL_PATH}" ]]; then
    echo "Using installed model: ${MODEL_INSTALL_PATH}"
else
    echo "Downloading model to ${MODEL_INSTALL_PATH}..."
    "${SUDO[@]}" mkdir -p "/Library/Application Support/llavon-ime/models"
    MODEL_PART_PATH="${TMPDIR:-/tmp}/${MODEL_FILE}.part"
    trap 'rm -f "${MODEL_PART_PATH}"' EXIT
    curl --fail --location --retry 3 --output "${MODEL_PART_PATH}" "${MODEL_URL}"
    test -s "${MODEL_PART_PATH}"
    "${SUDO[@]}" install -m 0644 "${MODEL_PART_PATH}" "${MODEL_INSTALL_PATH}"
    rm -f "${MODEL_PART_PATH}"
    trap - EXIT
fi

LLAVON_DEBUG_FLAG=""
if [[ -n "${LLAVON_IME_DEBUG:-}" ]]; then
    LLAVON_DEBUG_FLAG="-DLLAVON_IME_DEBUG=ON"
fi

# The running binary of a process, even after the file it was started from was
# replaced: the text vnode still maps the old inode.
process_text_inode() {
    local pid="$1" inode="" line
    while IFS= read -r line; do
        case "${line}" in
            i*) inode="${line#i}" ;;
            n*/llavon-ime-lora-gui) printf '%s\n' "${inode}"; return 0 ;;
        esac
    done < <(lsof -p "${pid}" -a -d txt -F in 2>/dev/null)
    return 1
}

# A manager from an older build cannot replace itself; stop it (unless it is
# training) so the next open uses the new build. A page that is still open
# keeps its address: the reinstalled manager hands the socket and the token
# over, and the page reloads when it sees the new build.
restart_stale_lora_manager() {
    local gui_path="${LLAVON_IME_LORA_GUI_PATH:-${INSTALL_PREFIX}/bin/llavon-ime-lora-gui}"
    local state_dir="${LLAVON_IME_LORA_STATE_DIR:-}"
    local installed_inode pid exe text_inode url token endpoint status
    if [[ ! -f "${gui_path}" ]]; then
        return 0
    fi
    # The manager keeps gui.lock in the per-user training state; the layout
    # matches the GUI's default_state() on macOS.
    if [[ -z "${state_dir}" ]]; then
        if [[ -n "${XDG_STATE_HOME:-}" ]]; then
            state_dir="${XDG_STATE_HOME}/llavon-ime/training"
        else
            state_dir="${HOME}/Library/Application Support/llavon-ime/training"
        fi
    fi
    installed_inode="$(stat -f '%i' "${gui_path}" 2>/dev/null || true)"
    sleep 2  # give a running manager time to hand itself over
    for pid in $(pgrep -f 'llavon-ime-lora-gui' 2>/dev/null || true); do
        exe="$(ps -p "${pid}" -o comm= 2>/dev/null | sed 's/^ *//')"
        case "${exe}" in
            "${gui_path}" | */llavon-ime-lora-gui) ;;
            *) continue ;;
        esac
        if [[ -n "${installed_inode}" ]]; then
            text_inode="$(process_text_inode "${pid}" || true)"
            if [[ "${text_inode}" == "${installed_inode}" ]]; then
                continue  # already the reinstalled build
            fi
        fi
        # A native manager owns a separate activation lock, and may be training.
        # It has no HTTP status endpoint; leave it open until the user closes it.
        if [[ -f "${state_dir}/native-gui.lock" ]]; then
            echo "Native LoRA manager (PID ${pid}) keeps running; reopen it after its work finishes to load the update." >&2
            continue
        fi
        url="$(head -n 1 "${state_dir}/gui.lock" 2>/dev/null || true)"
        if [[ "${url}" == http://127.0.0.1:* ]]; then
            token="${url#*#}"
            endpoint="${url%%#*}"
            status="$(curl -s --max-time 2 -H "X-Llavon-Token: ${token}" "${endpoint}api/state" || true)"
            case "${status}" in
                *'"state":"running"'*)
                    echo "LoRA manager (PID ${pid}) is training; it keeps running and is replaced afterwards." >&2
                    continue ;;
            esac
        fi
        if kill "${pid}" 2>/dev/null; then
            echo "Stopped the stale LoRA manager (PID ${pid}); the next open uses the new build."
        else
            echo "Could not stop the stale LoRA manager (PID ${pid}); close it manually." >&2
        fi
    done
}

echo "Building and testing ime-unix-service (Metal; the first build can take a while)..."
(
    cd "${ROOT_DIR}/ime-unix-service"
    cmake --preset macos -DIME_UNIX_SERVICE_BUILD_TESTS=ON \
        -DLLAVON_IME_INSTALLED_LORA_TRAINER_PATH="${LORA_TRAINER_DIR}/llavon-lora" \
        ${LLAVON_DEBUG_FLAG}
    cmake --build --preset macos --parallel
    ctest --test-dir build/macos --output-on-failure
)

echo "Installing ime-unix-service into ${INSTALL_PREFIX}..."
if [[ "${INSTALL_PREFIX}" == "${HOME}"/* ]]; then
    cmake --install "${ROOT_DIR}/ime-unix-service/build/macos" --prefix "${INSTALL_PREFIX}"
else
    # The package payload lives outside the home directory.
    "${SUDO[@]}" cmake --install "${ROOT_DIR}/ime-unix-service/build/macos" --prefix "${INSTALL_PREFIX}"
fi

if [[ -z "${LLAVON_IME_SKIP_LORA_TRAINER:-}" ]]; then
    echo "Downloading the pinned LoRA Trainer release into ${LORA_TRAINER_DIR}..."
    # The manager installs the whole verified release; a shared copy needs
    # readable modes, hence the same download the package bundles.
    if [[ "${LORA_TRAINER_DIR}" == "${HOME}"/* ]]; then
        mkdir -p "${LORA_TRAINER_DIR}"
        "${ROOT_DIR}/ime-unix-service/build/macos/llavon-ime-lora" \
            install-trainer --output-dir "${LORA_TRAINER_DIR}"
    else
        "${SUDO[@]}" mkdir -p "${LORA_TRAINER_DIR}"
        "${SUDO[@]}" "${ROOT_DIR}/ime-unix-service/build/macos/llavon-ime-lora" \
            install-trainer --output-dir "${LORA_TRAINER_DIR}"
        "${SUDO[@]}" chmod -R a+rX "${LORA_TRAINER_DIR}"
        "${SUDO[@]}" chmod 0644 "${LORA_TRAINER_DIR}/trainer-release.json"
    fi
else
    echo "Skipping the LoRA Trainer download (LLAVON_IME_SKIP_LORA_TRAINER is set)."
fi

if [[ -z "${LLAVON_IME_SKIP_LORA_GUI_RESTART:-}" ]]; then
    restart_stale_lora_manager
fi

if [[ "${LLAVON_IME_SERVICE_SKIP_NEXT_STEPS:-}" != "1" ]]; then
    cat <<EOF

Service build, tests, and installation completed successfully.

Next steps:
  * Build and install the input method frontend:
      macos/scripts/build-native-app.sh --install
  * The app finds the service at ${INSTALL_PREFIX}/bin/llavon-ime-unix-service
    and the tables at ${INSTALL_PREFIX}/share/llavon-ime/tables.
  * The pinned LoRA Trainer is at ${LORA_TRAINER_DIR}.
EOF
fi
