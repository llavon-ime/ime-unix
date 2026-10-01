#!/usr/bin/env python3
"""Generate the macOS-only Sparkle feed after the notarized pkg is published."""

import argparse
import base64
from datetime import datetime, timezone
from pathlib import Path
import re
import xml.etree.ElementTree as ET

SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
ET.register_namespace("sparkle", SPARKLE)


def version_tuple(version):
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("version must use major.minor.patch")
    return tuple(map(int, version.split(".")))


def generate(version, package, signature, repository, previous=None):
    version_tuple(version)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("invalid GitHub repository")
    if len(base64.b64decode(signature, validate=True)) != 64:
        raise ValueError("invalid Ed25519 signature")
    package = Path(package)
    if package.name != f"llavon-ime-{version}-arm64.pkg" or package.stat().st_size == 0:
        raise ValueError("expected a non-empty arm64 release pkg")
    root = ET.Element("rss", version="2.0")
    channel = ET.SubElement(root, "channel")
    ET.SubElement(channel, "title").text = "拉風輸入法 macOS 更新"
    ET.SubElement(channel, "link").text = f"https://github.com/{repository}"
    ET.SubElement(channel, "language").text = "zh-TW"
    item = ET.Element("item")
    ET.SubElement(item, "title").text = f"拉風輸入法 {version}"
    ET.SubElement(item, "pubDate").text = datetime.now(timezone.utc).strftime("%a, %d %b %Y %H:%M:%S GMT")
    ET.SubElement(item, f"{{{SPARKLE}}}version").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}shortVersionString").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}minimumSystemVersion").text = "13.0"
    # Inline plain text avoids loading remote web content in the IME process.
    ET.SubElement(item, "description").text = "更新輸入法、預測服務與套件內附工具。安裝需要管理員授權。"
    ET.SubElement(item, "enclosure", {
        "url": f"https://github.com/{repository}/releases/download/v{version}/{package.name}",
        "length": str(package.stat().st_size),
        "type": "application/octet-stream",
        f"{{{SPARKLE}}}installationType": "package",
        f"{{{SPARKLE}}}edSignature": signature,
    })
    items = {version: item}
    if previous is not None:
        old_root = ET.parse(previous).getroot()
        for old in old_root.findall("./channel/item"):
            old_version = old.findtext(f"{{{SPARKLE}}}version")
            version_tuple(old_version or "")
            enclosure = old.find("enclosure")
            expected_url = f"https://github.com/{repository}/releases/download/v{old_version}/llavon-ime-{old_version}-arm64.pkg"
            if enclosure is None or enclosure.get("url") != expected_url:
                raise ValueError("previous feed contains an unexpected package URL")
            if enclosure.get(f"{{{SPARKLE}}}installationType") != "package":
                raise ValueError("previous feed contains a non-package update")
            if old_version == version:
                if enclosure.attrib != item.find("enclosure").attrib:
                    raise ValueError("a published version cannot be replaced with a different package")
            items[old_version] = old
    for entry in sorted(items, key=version_tuple, reverse=True)[:20]:
        channel.append(items[entry])
    ET.indent(root, space="  ")
    return ET.ElementTree(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--signature", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    tree = generate(args.version, args.package, args.signature, args.repository, args.previous)
    tree.write(args.output, encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    main()
