"""Static gates for the pinned ARMv7M FPB/DebugMonitor backport."""
from hashlib import sha256
import json
from pathlib import Path
import unittest

from tools.qemu_backport import _sections


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "qemu/machine/patches/armv7m-fpb"


class Armv7mFpbPackageTests(unittest.TestCase):
    def test_manifest_matches_patch_and_file_sections(self):
        manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
        payload = (PACKAGE / manifest["patch"]).read_bytes()
        self.assertEqual(sha256(payload).hexdigest(), manifest["patch_sha256"])
        sections = _sections(payload)
        self.assertEqual(set(sections), {item["path"] for item in manifest["files"]})
        self.assertEqual(
            set(sections),
            {
                "hw/arm/armv7m.c",
                "hw/intc/armv7m_nvic.c",
                "include/hw/intc/armv7m_nvic.h",
                "target/arm/debug_helper.c",
            },
        )

    def test_build_pipeline_applies_fpb_after_nvic_dependencies(self):
        source = (ROOT / "tools/integrate_qemu.py").read_text(encoding="utf-8")
        dwt = source.index("('armv7m-dwt', 'ARMv7M DWT')")
        cache = source.index("('armv7m-cache-maintenance', 'ARMv7M cache maintenance')")
        fpb = source.index("('armv7m-fpb', 'ARMv7M FPB/DEMCR')")
        sd = source.index("('sd-read-ahead', 'SD/eMMC host read-ahead')")
        self.assertLess(dwt, fpb)
        self.assertLess(cache, fpb)
        self.assertLess(fpb, sd)


if __name__ == "__main__":
    unittest.main()
