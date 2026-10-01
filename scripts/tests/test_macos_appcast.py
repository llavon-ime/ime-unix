import base64
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("appcast", Path(__file__).resolve().parents[1] / "macos-appcast.py")
APPCAST = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(APPCAST)
SIGNATURE = base64.b64encode(bytes(range(64))).decode()


class AppcastTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def feed(self, version, signature=SIGNATURE, previous=None, content=b"test pkg"):
        package = self.root / f"llavon-ime-{version}-arm64.pkg"
        package.write_bytes(content)
        return APPCAST.generate(version, package, signature, "llavon-ime/ime-unix", previous)

    def save(self, feed):
        previous = self.root / "previous.xml"
        feed.write(previous, encoding="utf-8", xml_declaration=True)
        return previous

    def versions(self, feed):
        return [item.findtext(f"{{{APPCAST.SPARKLE}}}version") for item in feed.findall("./channel/item")]

    def test_signed_package_metadata(self):
        item = self.feed("1.2.3").find("./channel/item")
        self.assertEqual(item.findtext(f"{{{APPCAST.SPARKLE}}}minimumSystemVersion"), "13.0")
        enclosure = item.find("enclosure")
        self.assertEqual(enclosure.get(f"{{{APPCAST.SPARKLE}}}installationType"), "package")
        self.assertEqual(enclosure.get(f"{{{APPCAST.SPARKLE}}}edSignature"), SIGNATURE)
        self.assertEqual(enclosure.get("length"), "8")
        self.assertTrue(enclosure.get("url").endswith("/v1.2.3/llavon-ime-1.2.3-arm64.pkg"))

    def test_old_release_rerun_cannot_downgrade_feed(self):
        previous = self.save(self.feed("1.10.0"))
        self.assertEqual(self.versions(self.feed("1.9.0", previous=previous)), ["1.10.0", "1.9.0"])

    def test_release_rerun_is_idempotent(self):
        previous = self.save(self.feed("1.2.3"))
        self.assertEqual(self.versions(self.feed("1.2.3", previous=previous)), ["1.2.3"])

    def test_changed_package_under_same_version_is_rejected(self):
        previous = self.save(self.feed("1.2.3"))
        changed_signature = base64.b64encode(bytes(reversed(range(64)))).decode()
        with self.assertRaises(ValueError):
            self.feed("1.2.3", signature=changed_signature, previous=previous)

    def test_invalid_signature_and_empty_package_are_rejected(self):
        for signature in ["garbage", base64.b64encode(bytes(32)).decode()]:
            with self.assertRaises(ValueError):
                self.feed("1.2.3", signature=signature)
        with self.assertRaises(ValueError):
            self.feed("1.2.3", content=b"")

    def test_previous_feed_cannot_change_download_origin(self):
        feed = self.feed("1.2.3")
        feed.find("./channel/item/enclosure").set("url", "https://other.example/update.pkg")
        with self.assertRaises(ValueError):
            self.feed("1.2.4", previous=self.save(feed))

    def test_preview_versions_are_not_published_to_stable(self):
        with self.assertRaises(ValueError):
            self.feed("1.2.3-preview")


if __name__ == "__main__":
    unittest.main()
