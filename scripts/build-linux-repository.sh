#!/usr/bin/env bash
set -euo pipefail
# Build a static repository tree; publish the whole tree to an HTTPS host.
# Native package managers, not a product-specific binary feed, verify updates.
if [[ $# != 4 ]]; then
    echo "Usage: $0 <deb|rpm> <package> <output-directory> <GPG-fingerprint>" >&2
    exit 2
fi
format="$1"; package="$(realpath "$2")"; output="$3"; key="$4"
if [[ ! "${key}" =~ ^[A-Fa-f0-9]{40}$ ]]; then
    echo "Use the complete 40-digit repository key fingerprint." >&2; exit 2
fi
key="${key^^}"
export LC_ALL=C
gpg --batch --list-secret-keys "${key}" >/dev/null
case "${format}" in
    deb) root="${output}/debian13/amd64" ;;
    rpm) root="${output}/fedora43/x86_64" ;;
    *) echo "Unsupported repository format: ${format}" >&2; exit 2 ;;
esac
if [[ -e "${root}" ]]; then echo "Output repository already exists: ${root}" >&2; exit 2; fi
mkdir -p "${root}/packages"
cp "${package}" "${root}/packages/"
copied="${root}/packages/$(basename "${package}")"
gpg --batch --armor --export "${key}" > "${output}/llavon-ime-repository.asc"
if [[ "${format}" == "deb" ]]; then
    test "$(dpkg-deb --field "${package}" Package)" = llavon-ime-fcitx5
    test "$(dpkg-deb --field "${package}" Architecture)" = amd64
    version="$(dpkg-deb --field "${package}" Version)"
    (cd "${root}" && dpkg-scanpackages --multiversion packages /dev/null > Packages)
    gzip -n -9 -c "${root}/Packages" > "${root}/Packages.gz"
    (cd "${root}" && apt-ftparchive \
        -o APT::FTPArchive::Release::Origin=LlavonIME \
        -o APT::FTPArchive::Release::Label=LlavonIME \
        -o APT::FTPArchive::Release::Architectures=amd64 \
        release . > Release)
    gpg --batch --yes --local-user "${key}" --digest-algo SHA256 --clearsign \
        --output "${root}/InRelease" "${root}/Release"
    gpg --batch --yes --local-user "${key}" --digest-algo SHA256 --armor --detach-sign \
        --output "${root}/Release.gpg" "${root}/Release"
else
    test "$(rpm -qp --qf '%{NAME}' "${package}")" = llavon-ime-fcitx5
    test "$(rpm -qp --qf '%{ARCH}' "${package}")" = x86_64
    version="$(rpm -qp --qf '%{VERSION}' "${package}")"
    rpmsign --define "_gpg_name ${key}" --define "_gpg_path ${GNUPGHOME:-${HOME}/.gnupg}" \
        --define "__gpg $(command -v gpg)" --addsign "${copied}"
    createrepo_c --checksum sha256 "${root}"
    gpg --batch --yes --local-user "${key}" --digest-algo SHA256 --armor --detach-sign \
        --output "${root}/repodata/repomd.xml.asc" "${root}/repodata/repomd.xml"
fi
if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Only stable major.minor.patch packages belong in the release repository." >&2; exit 2
fi
python3 - "${output}" "${root}" "${version}" "${key}" "${copied}" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
output, root, version, key, package = sys.argv[1:]
package = Path(package)
digest = hashlib.file_digest(package.open('rb'), 'sha256').hexdigest()
Path(root, 'snapshot.json').write_text(json.dumps({
    'version': version, 'fingerprint': key,
    'package': str(package.relative_to(output)), 'sha256': digest,
}, indent=2) + '\n')
PY
echo "Signed ${format} repository: ${root}"
