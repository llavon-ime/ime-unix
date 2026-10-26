import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "tag_release_notes", Path(__file__).resolve().parents[1] / "tag-release-notes.py"
)
TAG_RELEASE_NOTES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TAG_RELEASE_NOTES)


class TagReleaseNotesTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.git("init", "-q")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release@example.invalid")
        (self.root / "tracked.txt").write_text("release\n", encoding="utf-8")
        self.git("add", "tracked.txt")
        self.git("commit", "-qm", "Initial release")

    def git(self, *arguments):
        subprocess.run(["git", *arguments], cwd=self.root, check=True)

    def test_extracts_annotated_tag_subject_and_body(self):
        self.git(
            "tag",
            "-a",
            "v1.2.3",
            "-m",
            "改善自動更新",
            "-m",
            "- 顯示版本摘要\n- 修正輸入來源",
        )
        self.assertEqual(
            TAG_RELEASE_NOTES.extract("v1.2.3", cwd=self.root),
            "改善自動更新\n\n- 顯示版本摘要\n- 修正輸入來源",
        )

    def test_rejects_lightweight_tag(self):
        self.git("tag", "v1.2.3")
        with self.assertRaisesRegex(ValueError, "must be annotated"):
            TAG_RELEASE_NOTES.extract("v1.2.3", cwd=self.root)

    def test_rejects_non_release_tag(self):
        with self.assertRaisesRegex(ValueError, "must use"):
            TAG_RELEASE_NOTES.extract("preview", cwd=self.root)


if __name__ == "__main__":
    unittest.main()
