"""Source-only regression checks for local package preparation and private status."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from prepare_local_client import copy_input


class LocalSetupTests(unittest.TestCase):
    def test_existing_input_is_preserved_when_source_differs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / "source"
            destination = root / "destination"
            source.write_bytes(b"new input")
            destination.write_bytes(b"existing input")

            with self.assertRaises(ValueError):
                copy_input(source, destination)
            self.assertEqual(destination.read_bytes(), b"existing input")

    def test_private_status_uses_its_recorded_backup_after_source_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / "source"
            private = root / "private"
            module_relative = "X5/Binaries/Win64/XIGNCODE/x3_x64.xem"
            client_relative = "X5/Binaries/Win64/ArcheAgeChronicles.exe"
            original = b"synthetic original module"
            installed = b"synthetic installed module"
            client = b"synthetic client identity"
            def digest(data):
                return hashlib.sha256(data).hexdigest()

            for base in [source, private]:
                (base / module_relative).parent.mkdir(parents=True)
            (source / module_relative).write_bytes(b"source changed after the copy")
            (private / module_relative).write_bytes(installed)
            (private / client_relative).write_bytes(client)
            module_directory = (private / module_relative).parent
            backup = module_directory / "x3_x64.xem.local-original-backup"
            backup.write_bytes(original)
            manifest = {
                "version": 1, "source": str(source), "pak_mode": "Copy",
                "client_executable_sha256": digest(client),
                "files": [
                    {"relative_path": module_relative.replace("/", "\\"),
                     "sha256": digest(original)},
                    {"relative_path": client_relative.replace("/", "\\"),
                     "sha256": digest(client)},
                ],
            }
            (private / ".local-client-copy.json").write_text(json.dumps(manifest))
            state = {
                "version": 1, "phase": "installed", "source": str(source),
                "private_root": str(private), "original_sha256": digest(original),
                "shim_sha256": digest(installed),
            }
            (module_directory / ".x3_x64.local-shim-state.json").write_text(json.dumps(state))
            command = [
                "pwsh", "-NoProfile", "-File",
                str(ROOT / "tools/xigncode_shim_experiment/install_private_shim.ps1"),
                "-Action", "Status", "-PrivateRoot", str(private),
            ]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("experimental-shim", result.stdout)

            backup.write_bytes(b"unexpected private backup")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("hash differs", result.stderr)


if __name__ == "__main__":
    unittest.main()
