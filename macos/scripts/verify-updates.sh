#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${ROOT_DIR}/macos/scripts/sparkle-config.sh"
prepare_sparkle
work="$(mktemp -d "${TMPDIR:-/tmp}/llavon-updates.XXXXXX")"
trap 'rm -rf "${work}"' EXIT

python3 -m unittest discover -s "${ROOT_DIR}/scripts/tests" -p test_macos_appcast.py
swiftc -warnings-as-errors -parse-as-library \
    "${ROOT_DIR}/macos/Core/UpdateInstallationGate.swift" \
    "${ROOT_DIR}/macos/Tests/UpdateGateTests.swift" -o "${work}/gate-tests"
"${work}/gate-tests"

# Disposable keys, never the developer's Keychain or a real release key.
cat > "${work}/test-key.swift" <<'SWIFT'
import CryptoKit
import Foundation
let key = Curve25519.Signing.PrivateKey()
try key.rawRepresentation.base64EncodedData().write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
print(key.publicKey.rawRepresentation.base64EncodedString())
SWIFT
public_key="$(swift "${work}/test-key.swift" "${work}/private-key")"
package="${work}/llavon-ime-1.2.3-arm64.pkg"
printf 'signature test package\n' > "${package}"
signature="$("${SPARKLE_DIR}/bin/sign_update" --ed-key-file "${work}/private-key" -p "${package}")"
swift "${ROOT_DIR}/macos/scripts/verify-update-signature.swift" "${public_key}" "${signature}" "${package}"
python3 "${ROOT_DIR}/scripts/macos-appcast.py" --version 1.2.3 --package "${package}" \
    --signature "${signature}" --repository llavon-ime/ime-unix --output "${work}/appcast.xml"
"${SPARKLE_DIR}/bin/sign_update" --ed-key-file "${work}/private-key" "${work}/appcast.xml"
"${SPARKLE_DIR}/bin/sign_update" --ed-key-file "${work}/private-key" --verify "${work}/appcast.xml"
printf 'tampered\n' >> "${package}"
if swift "${ROOT_DIR}/macos/scripts/verify-update-signature.swift" "${public_key}" "${signature}" "${package}"; then
    echo "A tampered update package was accepted." >&2
    exit 1
fi
python3 - "${work}/appcast.xml" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
path.write_bytes(path.read_bytes().replace(b"<sparkle:version>1.2.3", b"<sparkle:version>9.9.9"))
PY
if "${SPARKLE_DIR}/bin/sign_update" --ed-key-file "${work}/private-key" --verify "${work}/appcast.xml"; then
    echo "A tampered feed was accepted." >&2
    exit 1
fi
echo "macOS update signing tests passed"
