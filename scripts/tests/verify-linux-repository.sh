#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d)"
trap 'gpgconf --homedir "${work}/keys" --kill all 2>/dev/null || true; rm -rf "${work}"' EXIT
export GNUPGHOME="${work}/keys"
mkdir -m700 "${GNUPGHOME}"
gpg --batch --passphrase '' --quick-generate-key 'Llavon repository test <test@example.invalid>' rsa3072 sign 1d
key="$(gpg --batch --with-colons --list-keys | awk -F: '$1 == "fpr" {print $10; exit}')"

mkdir -p "${work}/deb/DEBIAN" "${work}/deb/usr/share/llavon-repository-fixture"
cat > "${work}/deb/DEBIAN/control" <<'CONTROL'
Package: llavon-ime-fcitx5
Version: 1.2.3
Architecture: amd64
Maintainer: Test <test@example.invalid>
Description: Repository update test fixture
CONTROL
printf 'fixture\n' > "${work}/deb/usr/share/llavon-repository-fixture/version"
dpkg-deb --root-owner-group --build "${work}/deb" "${work}/llavon-ime-fcitx5_1.2.3_amd64.deb"
bash "${ROOT_DIR}/scripts/build-linux-repository.sh" deb "${work}/llavon-ime-fcitx5_1.2.3_amd64.deb" "${work}/repo" "${key}"

mkdir -p "${work}/rpm/SPECS" "${work}/rpm/SOURCES"
cat > "${work}/rpm/SPECS/fixture.spec" <<'SPEC'
Name: llavon-ime-fcitx5
Version: 1.2.3
Release: 1
Summary: Repository update test fixture
License: MIT
BuildArch: x86_64
%global debug_package %{nil}
%description
Disposable package for repository verification.
%install
mkdir -p %{buildroot}/usr/share/llavon-repository-fixture
printf 'fixture\n' > %{buildroot}/usr/share/llavon-repository-fixture/version
%files
/usr/share/llavon-repository-fixture/version
SPEC
# The fixture contains text only; allow generating its x86_64 metadata on an
# ARM test machine without claiming to cross-compile any executable code.
printf 'buildarch_compat: %s: x86_64\n' "$(uname -m)" > "${work}/rpmrc"
rpmbuild --rcfile "$(rpm --eval '%{_rpmconfigdir}')/rpmrc:${work}/rpmrc" \
    --target x86_64 -bb --define "_topdir ${work}/rpm" "${work}/rpm/SPECS/fixture.spec"
bash "${ROOT_DIR}/scripts/build-linux-repository.sh" rpm "${work}/rpm/RPMS/x86_64/llavon-ime-fcitx5-1.2.3-1.x86_64.rpm" "${work}/repo" "${key}"
gpg --verify "${work}/repo/debian13/amd64/InRelease"
gpg --verify "${work}/repo/fedora43/x86_64/repodata/repomd.xml.asc" "${work}/repo/fedora43/x86_64/repodata/repomd.xml"
rpm --dbpath "${work}/rpm-db" --import "${work}/repo/llavon-ime-repository.asc"
rpm --dbpath "${work}/rpm-db" --checksig "${work}/repo/fedora43/x86_64/packages/"*.rpm | grep -q 'signatures OK'

# APT runs against an isolated lists/cache/status tree, never the developer's
# configured repositories or installed packages. Signed indexes and package
# SHA256 hashes are validated by APT itself.
mkdir -p "${work}/apt/lists/partial" "${work}/apt/cache/archives/partial" "${work}/downloads"
touch "${work}/apt/status"
gpg --batch --export "${key}" > "${work}/public.gpg"
printf 'deb [signed-by=%s] file:%s/debian13/amd64 ./\n' "${work}/public.gpg" "${work}/repo" > "${work}/sources.list"
options=(-o "Dir::Etc::sourcelist=${work}/sources.list" -o Dir::Etc::sourceparts=-
         -o "Dir::State::lists=${work}/apt/lists" -o "Dir::State::status=${work}/apt/status"
         -o "Dir::Cache=${work}/apt/cache" -o APT::Architecture=amd64 -o APT::Sandbox::User=root)
apt-get "${options[@]}" update
(cd "${work}/downloads" && apt-get "${options[@]}" download llavon-ime-fcitx5)
cmp "${work}/downloads/"*.deb "${work}/llavon-ime-fcitx5_1.2.3_amd64.deb"

mkdir -p "${work}/dnf-downloads" "${work}/empty-repos"
dnf_options=(--installroot "${work}/dnf" --releasever 43 --forcearch x86_64
    --setopt="reposdir=${work}/empty-repos"
    --repofrompath="llavon,file://${work}/repo/fedora43/x86_64"
    --setopt=llavon.gpgcheck=1 --setopt=llavon.repo_gpgcheck=1
    --setopt="llavon.gpgkey=file://${work}/repo/llavon-ime-repository.asc" -y)
dnf "${dnf_options[@]}" makecache
dnf "${dnf_options[@]}" install --downloadonly --downloaddir "${work}/dnf-downloads" llavon-ime-fcitx5
cmp "${work}/dnf-downloads/"*.rpm "${work}/repo/fedora43/x86_64/packages/"*.rpm

# Metadata-only hosting points to separately published GitHub assets. Keep the
# local mode above intact, and verify URLs/hashes before stripping packages.
export LLAVON_REPOSITORY_RELEASE_URL="https://github.com/llavon-ime/ime-unix/releases/download/v1.2.3"
bash "${ROOT_DIR}/scripts/build-linux-repository.sh" deb "${work}/llavon-ime-fcitx5_1.2.3_amd64.deb" "${work}/external" "${key}"
bash "${ROOT_DIR}/scripts/build-linux-repository.sh" rpm "${work}/rpm/RPMS/x86_64/llavon-ime-fcitx5-1.2.3-1.x86_64.rpm" "${work}/external" "${key}"
gpg --verify "${work}/external/debian13/amd64/InRelease"
gpg --verify "${work}/external/fedora43/x86_64/repodata/repomd.xml.asc" "${work}/external/fedora43/x86_64/repodata/repomd.xml"
python3 - "${work}/external" "${LLAVON_REPOSITORY_RELEASE_URL}" <<'PY'
import gzip
import hashlib
from pathlib import Path
import sys
import xml.etree.ElementTree as ET
root, url = Path(sys.argv[1]), sys.argv[2]
fields = dict(line.split(': ', 1) for line in (root / 'debian13/amd64/Packages').read_text().splitlines() if ': ' in line)
assert fields['Filename'] == url + '/llavon-ime-fcitx5_1.2.3_amd64.deb'
deb = root / 'debian13/amd64/packages/llavon-ime-fcitx5_1.2.3_amd64.deb'
assert fields['SHA256'] == hashlib.sha256(deb.read_bytes()).hexdigest()
rpmroot = root / 'fedora43/x86_64'
metadata = ET.parse(rpmroot / 'repodata/repomd.xml')
repo_ns = {'r': 'http://linux.duke.edu/metadata/repo'}
primary = metadata.find("r:data[@type='primary']/r:location", repo_ns)
tree = ET.fromstring(gzip.decompress((rpmroot / primary.attrib['href']).read_bytes()))
ns = {'c': 'http://linux.duke.edu/metadata/common'}
package = tree.find('c:package', ns)
location = package.find('c:location', ns)
assert location.attrib['{http://www.w3.org/XML/1998/namespace}base'] == url + '/'
assert location.attrib['href'] == 'llavon-ime-fcitx5-1.2.3-1.x86_64-signed.rpm'
assert package.find('c:checksum', ns).text == hashlib.sha256((rpmroot / location.attrib['href']).read_bytes()).hexdigest()
PY
unset LLAVON_REPOSITORY_RELEASE_URL

# A signed index referencing a modified package must fail integrity verification.
printf 'tampered\n' >> "${work}/repo/debian13/amd64/packages/"*.deb
rm "${work}/downloads/"*.deb
if (cd "${work}/downloads" && apt-get "${options[@]}" download llavon-ime-fcitx5); then
    echo 'APT accepted a tampered package.' >&2; exit 1
fi
sed -i 's/LlavonIME/LlavonBAD/' "${work}/repo/debian13/amd64/InRelease"
if gpg --verify "${work}/repo/debian13/amd64/InRelease"; then
    echo 'GPG accepted modified APT metadata.' >&2; exit 1
fi
printf '\n<!-- changed -->\n' >> "${work}/repo/fedora43/x86_64/repodata/repomd.xml"
if gpg --verify "${work}/repo/fedora43/x86_64/repodata/repomd.xml.asc" "${work}/repo/fedora43/x86_64/repodata/repomd.xml"; then
    echo 'GPG accepted modified DNF metadata.' >&2; exit 1
fi
echo 'Signed Linux repository verification passed'
