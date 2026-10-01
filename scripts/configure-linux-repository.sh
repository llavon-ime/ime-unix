#!/usr/bin/env bash
set -euo pipefail
# Explicit first-time enrollment. Updates subsequently use APT/DNF/PackageKit.
if [[ $# != 3 ]]; then
    echo "Usage: sudo $0 <HTTPS-repository-root> <public-key-file> <trusted-GPG-fingerprint>" >&2; exit 2
fi
url="${1%/}"; public_key="$2"; fingerprint="${3^^}"
if [[ ! "${url}" =~ ^https://[A-Za-z0-9.-]+(/[A-Za-z0-9._~-]+)*$ ||
      ! "${fingerprint}" =~ ^[A-F0-9]{40}$ ]]; then
    echo "Use an HTTPS repository root and a complete trusted key fingerprint." >&2; exit 2
fi
if [[ "$(id -u)" != 0 ]]; then echo "Repository enrollment requires administrator privileges." >&2; exit 2; fi
source /etc/os-release
case "${ID}:${VERSION_ID}:$(uname -m)" in
    debian:13:x86_64) format=deb ;;
    fedora:43:x86_64) format=rpm ;;
    *) echo "Official repositories currently support Debian 13 amd64 and Fedora 43 x86_64." >&2; exit 2 ;;
esac
work="$(mktemp -d)"; trap 'rm -rf "${work}"' EXIT
chmod 700 "${work}"
export GNUPGHOME="${work}"
gpg --batch --import "${public_key}" >/dev/null 2>&1
actual="$(gpg --batch --with-colons --fingerprint --list-keys | awk -F: '$1 == "pub" {want=1; next} want && $1 == "fpr" {print $10; want=0}')"
if [[ "${actual}" != "${fingerprint}" ]]; then
    echo "Public key does not match the trusted fingerprint, or contains multiple primary keys." >&2; exit 1
fi
if [[ "${format}" == deb ]]; then
    gpg --batch --export "${fingerprint}" > "${work}/key.gpg"
    install -Dm644 "${work}/key.gpg" /usr/share/keyrings/llavon-ime-repository.gpg
    cat > /etc/apt/sources.list.d/llavon-ime.sources <<EOF
Types: deb
URIs: ${url}/debian13/amd64/
Suites: ./
Architectures: amd64
Signed-By: /usr/share/keyrings/llavon-ime-repository.gpg
EOF
    apt-get update
else
    gpg --batch --armor --export "${fingerprint}" > "${work}/key.asc"
    install -Dm644 "${work}/key.asc" /etc/pki/rpm-gpg/LLAVON-IME-REPOSITORY
    cat > /etc/yum.repos.d/llavon-ime.repo <<EOF
[llavon-ime]
name=Llavon IME Fedora 43
baseurl=${url}/fedora43/x86_64/
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/LLAVON-IME-REPOSITORY
EOF
    dnf makecache --repo=llavon-ime
fi
echo "Repository configured. The settings app can now check and install native package updates."
