#!/usr/bin/env bash
# Shared, pinned dependency for app builds and release signing.
SPARKLE_VERSION="2.10.0"
SPARKLE_SHA256="c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c"
SPARKLE_DIR="${LLAVON_IME_SPARKLE_DIR:-${ROOT_DIR}/build/sparkle-${SPARKLE_VERSION}}"

prepare_sparkle() {
    local archive="${ROOT_DIR}/build/Sparkle-${SPARKLE_VERSION}.tar.xz"
    local staging
    if [[ -f "${SPARKLE_DIR}/.verified-${SPARKLE_SHA256}" &&
          -f "${SPARKLE_DIR}/Sparkle.framework/Sparkle" &&
          -x "${SPARKLE_DIR}/bin/sign_update" ]]; then
        return
    fi
    mkdir -p "${ROOT_DIR}/build" "$(dirname "${SPARKLE_DIR}")"
    if [[ ! -f "${archive}" ]] ||
       ! printf '%s  %s\n' "${SPARKLE_SHA256}" "${archive}" | shasum -a 256 --check --status; then
        curl --fail --location --retry 3 \
            "https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz" \
            -o "${archive}.partial"
        printf '%s  %s\n' "${SPARKLE_SHA256}" "${archive}.partial" | shasum -a 256 --check
        mv "${archive}.partial" "${archive}"
    fi
    staging="$(mktemp -d "${SPARKLE_DIR}.XXXXXX")"
    tar -xf "${archive}" -C "${staging}"
    test -f "${staging}/Sparkle.framework/Sparkle"
    test -x "${staging}/bin/sign_update"
    touch "${staging}/.verified-${SPARKLE_SHA256}"
    rm -rf "${SPARKLE_DIR}"
    mv "${staging}" "${SPARKLE_DIR}"
}

# Sign nested Sparkle code inside-out, using our identity for library validation.
sign_sparkle() {
    local framework="$1"
    local identity="$2"
    local version="${framework}/Versions/B"
    local item
    local options=(--force --sign "${identity}")
    if [[ "${identity}" == "-" ]]; then
        options+=(--timestamp=none)
    else
        options+=(--timestamp --options runtime)
    fi
    for item in "${version}/XPCServices/Downloader.xpc" \
                "${version}/XPCServices/Installer.xpc" \
                "${version}/Autoupdate" "${version}/Updater.app" "${framework}"; do
        codesign "${options[@]}" "${item}"
    done
}
