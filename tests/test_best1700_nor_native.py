"""Run the standalone NOR die tests with memory/undefined-behavior checking."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class Best1700NorNative(unittest.TestCase):
    @unittest.skipIf(os.name == "nt", "native sanitizer test runs in the Linux build environment")
    def test_die_protocol_and_borrowed_storage(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="best1700-nor-native-") as tmp:
            executable = Path(tmp) / "nor-test"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(root / "qemu/soc/best1700/include"),
                str(root / "qemu/soc/best1700/package/nor/best1700_nor.c"),
                str(root / "tests/test_best1700_nor.c"), "-o", str(executable),
            ], check=True, timeout=60)
            subprocess.run([str(executable)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
