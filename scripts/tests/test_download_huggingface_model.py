import importlib.util
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location(
    "download_huggingface_model",
    Path(__file__).resolve().parents[1] / "download-huggingface-model.py",
)
DOWNLOAD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DOWNLOAD)


class HuggingFaceModelMetadataTests(unittest.TestCase):
    def test_reads_resolved_revision_size_and_checksum(self):
        checksum = "a" * 64
        manifest = {
            "sha": "b" * 40,
            "siblings": [
                {"rfilename": "README.md", "size": 10},
                {
                    "rfilename": "model.gguf",
                    "size": 123,
                    "lfs": {"sha256": checksum},
                },
            ],
        }
        self.assertEqual(
            DOWNLOAD.model_file_metadata(manifest, "model.gguf"),
            ("b" * 40, 123, checksum),
        )

    def test_rejects_missing_file(self):
        with self.assertRaisesRegex(ValueError, "does not contain"):
            DOWNLOAD.model_file_metadata({"sha": "b" * 40, "siblings": []}, "model.gguf")

    def test_rejects_file_without_lfs_checksum(self):
        manifest = {
            "sha": "b" * 40,
            "siblings": [{"rfilename": "model.gguf", "size": 123}],
        }
        with self.assertRaisesRegex(ValueError, "LFS SHA-256"):
            DOWNLOAD.model_file_metadata(manifest, "model.gguf")

    def test_rejects_invalid_revision(self):
        manifest = {
            "sha": "main",
            "siblings": [
                {"rfilename": "model.gguf", "size": 123, "lfs": {"sha256": "a" * 64}}
            ],
        }
        with self.assertRaisesRegex(ValueError, "invalid model revision"):
            DOWNLOAD.model_file_metadata(manifest, "model.gguf")


if __name__ == "__main__":
    unittest.main()
