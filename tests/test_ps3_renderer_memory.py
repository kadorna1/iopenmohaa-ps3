#!/usr/bin/env python3
"""Regression checks for PS3 renderer-cache lifetime and zone diagnostics."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PS3RendererMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.renderer_init = (ROOT / "code/renderergl1/tr_init.c").read_text(encoding="utf-8")
        cls.renderer_image = (ROOT / "code/renderergl1/tr_image.c").read_text(encoding="utf-8")
        cls.memory = (ROOT / "code/qcommon/memory.c").read_text(encoding="utf-8")
        cls.workflow = (ROOT / ".github/workflows/ps3-build.yml").read_text(encoding="utf-8")

    def test_stale_images_are_freed_before_map_texture_warmup(self):
        start = self.renderer_init.index("void RE_EndRegistration( void )")
        end = self.renderer_init.index("void RE_SetRenderTime", start)
        registration = self.renderer_init[start:end]

        self.assertIn("R_FreeUnusedImages();", registration)
        self.assertIn("RB_PrecacheImages();", registration)
        self.assertLess(
            registration.index("R_FreeUnusedImages();"),
            registration.index("RB_PrecacheImages();"),
        )

    def test_texture_memory_report_sums_every_image(self):
        start = self.renderer_image.index("int R_CountTextureMemory()")
        end = self.renderer_image.index("void R_FreeUnusedImages()", start)
        report = self.renderer_image[start:end]

        self.assertIn("total_bytes += tr.images[i].bytesUsed;", report)
        self.assertNotIn("total_bytes = tr.images[i].bytesUsed;", report)

    def test_allocator_failure_reports_pool_breakdown_and_callsite(self):
        self.assertIn("ps3_zone_dump_tag_usage( \"malloc-failed\" )", self.memory)
        self.assertIn("ZONE TAGSUMMARY[%s]", self.memory)
        self.assertIn("tag=%s(%d) caller=%p hint=%p", self.memory)

    def test_zone_tag_cleanup_reports_released_bytes(self):
        self.assertIn("ZONE FREETAGS tag=%s id=%d blocks=%d bytes=%u", self.memory)
        self.assertIn("freedBytes += block->size;", self.memory)

    def test_workflow_runs_renderer_memory_regressions(self):
        self.assertIn("Test PS3 renderer cache and zone memory diagnostics", self.workflow)
        self.assertIn("test_ps3_renderer_memory.py", self.workflow)


if __name__ == "__main__":
    unittest.main()
