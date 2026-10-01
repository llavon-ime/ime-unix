#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL_FILE="llavon-ime-llama-250m-Q4_K_M.gguf"
MODEL_URL="${LLAVON_IME_MODEL_URL:-https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/resolve/main/${MODEL_FILE}}"
MODEL_DIR="${LLAVON_IME_MODEL_DIR:-${ROOT_DIR}/models}"
MODEL_PATH="${MODEL_DIR}/${MODEL_FILE}"
MODEL_INSTALL_PATH="/usr/share/llavon-ime/models/${MODEL_FILE}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script only supports Linux." >&2
    exit 2
fi

case "$(uname -m)" in
    x86_64 | amd64) ;;
    *)
        echo "The linux presets currently only support x86_64." >&2
        exit 2
        ;;
esac

for command in git cmake curl install pkg-config sed; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        exit 2
    fi
done

if ! pkg-config --exists atspi-2; then
    echo "Note: at-spi2-core development files not found; building without AT-SPI context support." >&2
    echo "      Install them (e.g. at-spi2-core-devel / libatspi2.0-dev / at-spi2-core) to enable it." >&2
fi

DISPLAY_VERSION="${LLAVON_IME_VERSION:-}"
if [[ -z "${DISPLAY_VERSION}" ]]; then
    describe="$(git -C "${ROOT_DIR}" describe --long --tags --abbrev=7 2>/dev/null || true)"
    if [[ -n "${describe}" ]]; then
        DISPLAY_VERSION="$(printf '%s\n' "${describe}" | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g')"
    else
        base_version="$(sed -n 's/^project(llavon-ime VERSION \([^ ]*\).*/\1/p' "${ROOT_DIR}/fcitx5/CMakeLists.txt")"
        base_version="${base_version:-0.1.0}"
        DISPLAY_VERSION="${base_version}.r$(git -C "${ROOT_DIR}" rev-list --count HEAD).g$(git -C "${ROOT_DIR}" rev-parse --short=7 HEAD)"
    fi
fi
echo "Llavon IME display version: ${DISPLAY_VERSION}"

SUDO=()
if ((EUID != 0)); then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "Required command not found: sudo" >&2
        exit 2
    fi
    SUDO=(sudo)
fi

if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ||
      ! -f "${ROOT_DIR}/ime-unix-service/CMakeLists.txt" ||
      ! -f "${ROOT_DIR}/ime-core/CMakeLists.txt" ||
      ! -f "${ROOT_DIR}/lora-trainer/.git" ]]; then
    echo "Initializing git submodules..."
    git -C "${ROOT_DIR}" submodule update --init --recursive
fi

if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    echo "Bootstrapping vcpkg..."
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

if [[ ! -s "${MODEL_PATH}" ]]; then
    echo "Downloading model to ${MODEL_PATH}..."
    mkdir -p "${MODEL_DIR}"
    MODEL_PART_PATH="${MODEL_PATH}.part"
    trap 'rm -f "${MODEL_PART_PATH}"' EXIT
    curl --fail --location --retry 3 --output "${MODEL_PART_PATH}" "${MODEL_URL}"
    test -s "${MODEL_PART_PATH}"
    mv "${MODEL_PART_PATH}" "${MODEL_PATH}"
    trap - EXIT
else
    echo "Using existing model: ${MODEL_PATH}"
fi

LLAVON_DEBUG_FLAG="-DLLAVON_IME_DEBUG=OFF"
if [[ -n "${LLAVON_IME_DEBUG:-}" ]]; then
    LLAVON_DEBUG_FLAG="-DLLAVON_IME_DEBUG=ON"
fi

# The pinned LoRA Trainer lives next to the private service payload, matching
# the Windows layout under the application directory.
PRIVATE_LIBDIR="/usr/lib"
if [[ -e /usr/lib64 ]]; then
    PRIVATE_LIBDIR="/usr/lib64"
fi
LORA_TRAINER_DIR="${PRIVATE_LIBDIR}/llavon-ime/tools/lora"
# The memory probe helper is resolved by the engine at runtime; the same path
# is compiled in below and used when installing the helper.
MEMSCAN_PATH="/usr/libexec/llavon-ime/llavon-ime-memscan"

echo "Building and testing ime-unix-service..."
(
    cd "${ROOT_DIR}/ime-unix-service"
    cmake --preset linux \
        -DIME_UNIX_SERVICE_BUILD_TESTS=ON \
        -DLLAVON_IME_INSTALLED_LORA_TRAINER_PATH="${LORA_TRAINER_DIR}/llavon-lora" \
        ${LLAVON_DEBUG_FLAG}
    cmake --build --preset linux --parallel
    ctest --test-dir build/linux --output-on-failure
)

echo "Building and testing fcitx5 addon..."
(
    cd "${ROOT_DIR}/fcitx5"
    cmake --preset linux \
        -DLLAVON_IME_INSTALLED_MODEL_PATH="${MODEL_INSTALL_PATH}" \
        -DLLAVON_IME_INSTALLED_MEMSCAN_PATH="${MEMSCAN_PATH}" \
        -DLLAVON_IME_DISPLAY_VERSION="${DISPLAY_VERSION}" \
        ${LLAVON_DEBUG_FLAG}
    cmake --build --preset linux --parallel
    ctest --preset linux
)

echo "Building and testing the memory probe helper..."
cmake -S "${ROOT_DIR}/memscan" -B "${ROOT_DIR}/build/memscan" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    ${LLAVON_DEBUG_FLAG}
cmake --build "${ROOT_DIR}/build/memscan" --parallel
ctest --test-dir "${ROOT_DIR}/build/memscan" --output-on-failure

echo "Installing ime-unix-service, fcitx5 addon, memory probe helper, and model..."
"${SUDO[@]}" cmake --install "${ROOT_DIR}/ime-unix-service/build/linux"
"${SUDO[@]}" cmake --install "${ROOT_DIR}/build/fcitx5"
"${SUDO[@]}" cmake --install "${ROOT_DIR}/build/memscan"
"${SUDO[@]}" install -Dm644 "${MODEL_PATH}" "${MODEL_INSTALL_PATH}"

# The manager watches its own binary: a reinstalled build replaces the running
# process and keeps the page address, and the page reloads itself when it sees
# the new build (the same idea as the memory helper restarting on reinstall).
# A manager from an older build that predates that behaviour cannot hand itself
# over; stop it so the next menu open uses the new build. A training job is
# never interrupted.
GUI_PATH="${LLAVON_IME_LORA_GUI_PATH:-/usr/bin/llavon-ime-lora-gui}"
if [[ -z "${LLAVON_IME_SKIP_LORA_GUI_RESTART:-}" ]]; then
    sleep 2  # give a running manager time to hand itself over
    installed_inode="$(stat -c '%i' "${GUI_PATH}" 2>/dev/null || true)"
    for pid in $(pgrep -f 'llavon-ime-lora-gui' 2>/dev/null || true); do
        exe="$(readlink "/proc/${pid}/exe" 2>/dev/null || true)"
        case "${exe}" in
            "${GUI_PATH}" | "${GUI_PATH} (deleted)") ;;
            *) continue ;;
        esac
        if [[ -n "${installed_inode}" &&
              "$(stat -Lc '%i' "/proc/${pid}/exe" 2>/dev/null || true)" == "${installed_inode}" ]]; then
            continue  # already the reinstalled build
        fi
        state_dir="${LLAVON_IME_LORA_STATE_DIR:-${XDG_STATE_HOME:-${HOME}/.local/state}/llavon-ime/training}"
        if [[ -f "${state_dir}/native-gui.lock" ]]; then
            echo "Native LoRA manager (PID ${pid}) keeps running; reopen it when its work is finished to load the update." >&2
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
fi

# Reading another process needs ptrace permission. CAP_SYS_PTRACE on the helper
# is the narrow option; kernel.yama.ptrace_scope=0 also works but applies to
# every process of the user.
if [[ -z "${LLAVON_IME_SKIP_MEMSCAN_CAPABILITY:-}" ]]; then
    if command -v setcap >/dev/null 2>&1; then
        "${SUDO[@]}" setcap cap_sys_ptrace+ep "${MEMSCAN_PATH}" || {
            echo "Could not grant CAP_SYS_PTRACE to the memory probe helper." >&2
            echo "Grant it manually or set kernel.yama.ptrace_scope=0 instead." >&2
        }
    else
        echo "setcap not found (install libcap); the memory context source stays unavailable." >&2
    fi
fi

if [[ -z "${LLAVON_IME_SKIP_LORA_TRAINER:-}" ]]; then
    echo "Downloading the pinned LoRA Trainer release..."
    # The manager installs the whole verified release; the system copy needs
    # readable modes because it is shared by every user.
    "${SUDO[@]}" mkdir -p "${LORA_TRAINER_DIR}"
    "${SUDO[@]}" "${ROOT_DIR}/ime-unix-service/build/linux/llavon-ime-lora" \
        install-trainer --output-dir "${LORA_TRAINER_DIR}"
    "${SUDO[@]}" chmod -R a+rX "${LORA_TRAINER_DIR}"
    "${SUDO[@]}" chmod 0644 "${LORA_TRAINER_DIR}/trainer-release.json"
else
    echo "Skipping the LoRA Trainer download (LLAVON_IME_SKIP_LORA_TRAINER is set)."
fi

echo "Linux build, tests, and installation completed successfully."
