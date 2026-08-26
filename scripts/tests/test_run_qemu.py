from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_QEMU = REPO_ROOT / "scripts" / "run_qemu.sh"


class RunQemuExitMappingTests(unittest.TestCase):
    def mapped_status(self, raw_status: int, *, mapping: bool = True) -> int:
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "guest.log"
            completed = subprocess.run(
                [
                    str(RUN_QEMU),
                    str(log),
                    "1" if mapping else "0",
                    "sh",
                    "-c",
                    f"exit {raw_status}",
                ],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return completed.returncode

    def test_debug_exit_statuses_map_to_the_guest_vocabulary(self) -> None:
        expected = {
            1: 0,
            3: 1,
            5: 3,
            7: 3,
            9: 4,
            11: 5,
            13: 6,
        }
        for raw, mapped in expected.items():
            with self.subTest(raw=raw):
                self.assertEqual(self.mapped_status(raw), mapped)

    def test_unmapped_status_is_preserved(self) -> None:
        self.assertEqual(self.mapped_status(17), 17)

    def test_mapping_can_be_disabled(self) -> None:
        self.assertEqual(self.mapped_status(7, mapping=False), 7)


if __name__ == "__main__":
    unittest.main()
