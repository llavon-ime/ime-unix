#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FORMAT="${1:-}"
VERSION="${LLAVON_IME_VERSION:-}"
MODEL_PATH="${LLAVON_IME_PACKAGE_MODEL_PATH:-}"
TRIPLET="x64-linux-llavon-prebuilt"
DIST_DIR="${LLAVON_IME_DIST_DIR:-${ROOT_DIR}/dist/linux}"

case "${FORMAT}" in
    deb)
        PACKAGE_ARCH="amd64"
        PRIVATE_LIBDIR="/usr/lib"
        LICENSE_DIR="/usr/share/doc/llavon-ime-fcitx5"
        ;;
    rpm)
        PACKAGE_ARCH="x86_64"
        PRIVATE_LIBDIR="/usr/lib64"
        LICENSE_DIR="/usr/share/licenses/llavon-ime-fcitx5"
        ;;
    *)
        echo "Usage: LLAVON_IME_VERSION=<version> $0 deb|rpm" >&2
        exit 2
        ;;
esac

if [[ -z "${VERSION}" || ! "${VERSION}" =~ ^[0-9][0-9A-Za-z._+~]*$ ]]; then
    echo "LLAVON_IME_VERSION must be a package-safe version without a leading v." >&2
    exit 2
fi

if [[ -z "${MODEL_PATH}" ]]; then
    shopt -s nullglob
    model_candidates=("${ROOT_DIR}"/models/*.gguf)
    shopt -u nullglob
    if [[ ${#model_candidates[@]} -eq 1 ]]; then
        MODEL_PATH="${model_candidates[0]}"
    fi
fi
if [[ ! -f "${MODEL_PATH}" || "${MODEL_PATH}" != *.gguf ]]; then
    echo "LLAVON_IME_PACKAGE_MODEL_PATH must point to one bundled .gguf model." >&2
    exit 2
fi

for command in cmake ninja patchelf file grep ldd; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Missing required command: ${command}" >&2
        exit 2
    fi
done
if [[ "${FORMAT}" == "deb" ]] && ! command -v dpkg-deb >/dev/null 2>&1; then
    echo "Missing required command: dpkg-deb" >&2
    exit 2
fi
if [[ "${FORMAT}" == "rpm" ]] && ! command -v rpmbuild >/dev/null 2>&1; then
    echo "Missing required command: rpmbuild" >&2
    exit 2
fi

if [[ ! -f "${ROOT_DIR}/ime-core/CMakeLists.txt" ||
      ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ||
      ! -f "${ROOT_DIR}/lora-trainer/.git" ]]; then
    git -C "${ROOT_DIR}" submodule update --init ime-core vcpkg lora-trainer
fi
if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    rm -f "${ROOT_DIR}/vcpkg/vcpkg"
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

BUILD_ROOT="${ROOT_DIR}/build/package-linux-${FORMAT}"
SERVICE_BUILD_DIR="${BUILD_ROOT}/service"
ADDON_BUILD_DIR="${BUILD_ROOT}/addon"
PKGROOT="${BUILD_ROOT}/pkgroot"
PRIVATE_BINDIR="${PRIVATE_LIBDIR#/usr/}/llavon-ime"
MODEL_INSTALL_PATH="/usr/share/llavon-ime/models/$(basename "${MODEL_PATH}")"

rm -rf "${BUILD_ROOT}"
mkdir -p "${PKGROOT}" "${DIST_DIR}"

cmake \
    -S "${ROOT_DIR}/ime-unix-service" \
    -B "${SERVICE_BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_BINDIR="${PRIVATE_BINDIR}" \
    -DCMAKE_INSTALL_RPATH='${ORIGIN}' \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
    -DVCPKG_OVERLAY_TRIPLETS="${ROOT_DIR}/ime-unix-service/triplets" \
    -DVCPKG_MANIFEST_FEATURES=llama-vulkan \
    -DIME_UNIX_SERVICE_BUILD_TESTS=ON \
    -DLLAVON_IME_INSTALLED_LORA_TRAINER_PATH="${PRIVATE_LIBDIR}/llavon-ime/tools/lora/llavon-lora"
cmake --build "${SERVICE_BUILD_DIR}"
ctest --test-dir "${SERVICE_BUILD_DIR}" --output-on-failure
DESTDIR="${PKGROOT}" cmake --install "${SERVICE_BUILD_DIR}"

cmake \
    -S "${ROOT_DIR}/fcitx5" \
    -B "${ADDON_BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_BINDIR="${PRIVATE_BINDIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
    -DVCPKG_OVERLAY_TRIPLETS="${ROOT_DIR}/ime-unix-service/triplets" \
    -DLLAVON_IME_BUILD_TESTS=ON \
    -DLLAVON_IME_REQUIRE_ATSPI=ON \
    -DLLAVON_IME_INSTALLED_MODEL_PATH="${MODEL_INSTALL_PATH}" \
    -DLLAVON_IME_DISPLAY_VERSION="${VERSION}"
cmake --build "${ADDON_BUILD_DIR}"
# Some distro devel packages expose TestFrontend headers/CMake metadata but
# omit the runtime test addon. The engine tests detect that and report
# themselves as skipped, so run the full suite.
ctest --test-dir "${ADDON_BUILD_DIR}" --output-on-failure
DESTDIR="${PKGROOT}" cmake --install "${ADDON_BUILD_DIR}"

# ime-core's development install rules are not part of the end-user package.
rm -rf \
    "${PKGROOT}/usr/include/ime-core" \
    "${PKGROOT}/usr/share/ime-core" \
    "${PKGROOT}/usr/share/licenses/ime-core" \
    "${PKGROOT}/usr/share/licenses/ime-unix-service"
find "${PKGROOT}/usr" -path '*/cmake/ime-core' -type d -prune -exec rm -rf {} +
find "${PKGROOT}/usr" -name 'libime-core.a' -delete

private_root="${PKGROOT}${PRIVATE_LIBDIR}/llavon-ime"
mkdir -p "${private_root}" "${PKGROOT}$(dirname "${MODEL_INSTALL_PATH}")"
install -m 0644 "${MODEL_PATH}" "${PKGROOT}${MODEL_INSTALL_PATH}"

vcpkg_prefix="${SERVICE_BUILD_DIR}/vcpkg_installed/${TRIPLET}"
shopt -s nullglob
runtime_libraries=(
    "${vcpkg_prefix}"/lib/*.so*
    "${vcpkg_prefix}"/bin/*.so*
)
shopt -u nullglob
if [[ ${#runtime_libraries[@]} -eq 0 ]]; then
    echo "No vcpkg runtime libraries were produced for ${TRIPLET}." >&2
    exit 1
fi
for library in "${runtime_libraries[@]}"; do
    case "$(basename "${library}")" in
        libvulkan.so*) continue ;;
    esac
    cp -a "${library}" "${private_root}/"
done

while IFS= read -r -d '' binary; do
    if file -b "${binary}" | grep -q '^ELF '; then
        patchelf --set-rpath '$ORIGIN' "${binary}"
    fi
done < <(find "${private_root}" -type f -print0)

license_root="${PKGROOT}${LICENSE_DIR}"
cmake \
    -DVCPKG_INSTALLED_DIR="${SERVICE_BUILD_DIR}/vcpkg_installed" \
    -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
    -DDESTINATION="${license_root}" \
    -DPROJECT_ROOT="${ROOT_DIR}" \
    -P "${ROOT_DIR}/scripts/install-licenses.cmake"

# The bundled LoRA trainer ships its own license in the release archive.
if [[ -f "${private_root}/llavon-ime/tools/lora/LICENSE" ]]; then
    install -Dm0644 "${private_root}/llavon-ime/tools/lora/LICENSE" \
        "${license_root}/llavon-lora-trainer/LICENSE"
fi

addon_path="$(find "${PKGROOT}/usr" -path '*/fcitx5/llavon-ime-addon.so' -print -quit)"
required_files=(
    "${private_root}/llavon-ime-unix-service"
    "${private_root}/llavon-ime-lora"
    "${private_root}/llavon-ime-lora-gui"
    "${private_root}/llavon-ime-lora-backend"
    "${private_root}/atspi_probe"
    "${addon_path}"
    "${PKGROOT}/usr/share/fcitx5/addon/llavon-ime.conf"
    "${PKGROOT}/usr/share/fcitx5/inputmethod/llavon-ime.conf"
    "${PKGROOT}/usr/share/llavon-ime/tables/bopomofo_char.json"
    "${PKGROOT}${MODEL_INSTALL_PATH}"
    "${license_root}/llavon-ime/LICENSE"
    "${license_root}/llavon-ime-model/NOTICE"
)
for required_file in "${required_files[@]}"; do
    if [[ -z "${required_file}" || ! -f "${required_file}" ]]; then
        echo "Missing package payload file: ${required_file}" >&2
        exit 1
    fi
done

if ! grep -a -q 'libatspi.so.0' "${addon_path}"; then
    echo "The packaged addon does not contain the AT-SPI runtime backend." >&2
    exit 1
fi
bash "${ROOT_DIR}/scripts/verify-linux-cpu-backends.sh" "${private_root}"

elf_files=("${addon_path}")
while IFS= read -r -d '' binary; do
    elf_files+=("${binary}")
done < <(find "${private_root}" -type f -print0)
for binary in "${elf_files[@]}"; do
    if file -b "${binary}" | grep -q '^ELF '; then
        if LD_LIBRARY_PATH="${private_root}" ldd "${binary}" | grep -q 'not found'; then
            echo "Unresolved runtime dependency in ${binary}:" >&2
            LD_LIBRARY_PATH="${private_root}" ldd "${binary}" >&2
            exit 1
        fi
    fi
done

echo "Bundling the pinned LoRA Trainer release..."
trainer_staging="$(mktemp -d "${TMPDIR:-/tmp}/llavon-lora-package.XXXXXX")"
trap 'rm -rf "${trainer_staging}"' EXIT
"${SERVICE_BUILD_DIR}/llavon-ime-lora" install-trainer --output-dir "${trainer_staging}"
mkdir -p "${private_root}/tools/lora"
cp -a "${trainer_staging}/." "${private_root}/tools/lora/"
# The pinned trainer release keeps the absolute runpath of its build machine
# on its native libraries, and rpm's check-rpaths rejects the package for it.
# The libraries all live in this directory, so $ORIGIN is the correct search
# path for every one of them.
while IFS= read -r -d '' binary; do
    if file -b "${binary}" | grep -q '^ELF '; then
        patchelf --set-rpath '$ORIGIN' "${binary}"
    fi
done < <(find "${private_root}/tools/lora" -type f -print0)
chmod -R a+rX "${private_root}/tools/lora"
chmod 0644 "${private_root}/tools/lora/trainer-release.json"
for trainer_file in "${private_root}/tools/lora/llavon-lora" "${private_root}/tools/lora/trainer-release.json"; do
    if [[ ! -f "${trainer_file}" ]]; then
        echo "Missing packaged LoRA Trainer file: ${trainer_file}" >&2
        exit 1
    fi
done

if [[ "${FORMAT}" == "deb" ]]; then
    control_dir="${PKGROOT}/DEBIAN"
    mkdir -p "${control_dir}"
    installed_size="$(du -sk "${PKGROOT}/usr" | cut -f1)"
    sed \
        -e "s/@VERSION@/${VERSION}/g" \
        -e "s/@INSTALLED_SIZE@/${installed_size}/g" \
        "${ROOT_DIR}/packaging/linux/debian/control.in" > "${control_dir}/control"
    package_path="${DIST_DIR}/llavon-ime-fcitx5_${VERSION}_${PACKAGE_ARCH}.deb"
    dpkg-deb --root-owner-group --build "${PKGROOT}" "${package_path}"
else
    rpm_root="${BUILD_ROOT}/rpmbuild"
    mkdir -p "${rpm_root}/BUILD" "${rpm_root}/BUILDROOT" "${rpm_root}/RPMS" \
        "${rpm_root}/SOURCES" "${rpm_root}/SPECS" "${rpm_root}/SRPMS"
    ln -s "${PKGROOT}" "${rpm_root}/SOURCES/pkgroot"
    sed \
        -e "s/@VERSION@/${VERSION}/g" \
        -e "s|@PRIVATE_LIBDIR@|${PRIVATE_LIBDIR}|g" \
        "${ROOT_DIR}/packaging/linux/rpm/llavon-ime-fcitx5.spec.in" \
        > "${rpm_root}/SPECS/llavon-ime-fcitx5.spec"
    rpmbuild -bb --define "_topdir ${rpm_root}" "${rpm_root}/SPECS/llavon-ime-fcitx5.spec"
    package_path="$(find "${rpm_root}/RPMS" -name '*.rpm' -print -quit)"
    cp "${package_path}" "${DIST_DIR}/"
    package_path="${DIST_DIR}/$(basename "${package_path}")"
fi

echo "Built package: ${package_path}"
