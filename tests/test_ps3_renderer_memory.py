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
        cls.texture_backend = (ROOT / "code/gl/ps3gl_textures.c").read_text(encoding="utf-8")
        cls.texture_header = (ROOT / "code/gl/ps3gl.h").read_text(encoding="utf-8")
        cls.world_bsp = (ROOT / "code/renderergl1/tr_bsp.c").read_text(encoding="utf-8")
        cls.cgame = (ROOT / "code/client/cl_cgame.cpp").read_text(encoding="utf-8")
        cls.ps3_step = (ROOT / "code/sys/ps3_step.h").read_text(encoding="utf-8")
        cls.memory = (ROOT / "code/qcommon/memory.c").read_text(encoding="utf-8")
        cls.workflow = (ROOT / ".github/workflows/ps3-build.yml").read_text(encoding="utf-8")
        cls.autoexec = (ROOT / "assets/ps3_usrdir/main/autoexec.cfg").read_text(encoding="utf-8")
        cls.expansion_autoexecs = {
            name: (ROOT / f"assets/ps3_usrdir/{name}/autoexec.cfg").read_text(encoding="utf-8")
            for name in ("mainta", "maintt")
        }
        cls.archive_source = (ROOT / "code/fgame/archive.cpp").read_text(encoding="utf-8")
        cls.shader_source = (ROOT / "code/renderergl1/tr_shader.c").read_text(encoding="utf-8")

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
        self.assertIn('ps3_zone_dump_tag_usage( "malloc-failed", 1 )', self.memory)
        self.assertIn("ZONE TAGSUMMARY[%s]", self.memory)
        self.assertIn("tag=%s(%d) caller=%p hint=%p", self.memory)

    def test_zone_tag_cleanup_reports_released_bytes(self):
        self.assertIn("ZONE FREETAGS tag=%s id=%d blocks=%d bytes=%u", self.memory)
        self.assertIn("freedBytes += block->size;", self.memory)

    def test_ps3gl_uses_native_16_bit_formats_and_padded_pitch(self):
        self.assertIn("GCM_TEXTURE_FORMAT_R5G6B5", self.texture_backend)
        self.assertIn("GCM_TEXTURE_FORMAT_A4R4G4B4", self.texture_backend)
        self.assertIn("t->gcm_format | GCM_TEXTURE_FORMAT_LIN", self.texture_backend)
        self.assertIn("return (row_bytes + 63u) & ~63u;", self.texture_backend)
        self.assertIn("t->bpp, t->gcm_format", self.texture_backend)
        self.assertIn("#define PS3GL_TEXTURE_ALIGN 128", self.texture_backend)
        self.assertIn("rsxMemalign(PS3GL_TEXTURE_ALIGN, size)", self.texture_backend)
        self.assertIn("gcm_format; /* GCM_TEXTURE_FORMAT_* without LIN flag */", self.texture_header)

    def test_ps3_defaults_to_compact_textures_and_fast_lightmaps(self):
        self.assertIn('r_texturebits = ri.Cvar_Get( "r_texturebits", "16"', self.renderer_init)
        self.assertIn('r_fastdlights = ri.Cvar_Get("r_fastdlights", "1"', self.renderer_init)
        self.assertIn("PS3 renderer memory policy: texturebits=%d fastdlights=%d", self.renderer_init)
        fast_path = self.world_bsp.index("if (r_fastdlights->integer)")
        duplicate_copy = self.world_bsp.index("ri.Hunk_Alloc(l->length", fast_path)
        self.assertLess(fast_path, duplicate_copy)
        self.assertIn("s_worldData.lighting = NULL;", self.world_bsp)

    def test_archived_renderer_overrides_are_forced_by_ps3_autoexec(self):
        self.assertIn('seta r_texturebits "16"', self.autoexec)
        self.assertIn('seta r_fastdlights "1"', self.autoexec)

    def test_expansion_autoexecs_force_compact_renderer_settings(self):
        for name, config in self.expansion_autoexecs.items():
            with self.subTest(expansion=name):
                self.assertIn('seta r_texturebits "16"', config)
                self.assertIn('seta r_fastdlights "1"', config)
                self.assertIn('seta ps3_aim_assist "1"', config)
                self.assertNotIn("#", config)

    def test_ps3_save_archives_skip_second_full_size_compression_buffer(self):
        start = self.archive_source.index("qboolean ArchiveFile::Compress()")
        end = self.archive_source.index("size_t ArchiveFile::Length(void)", start)
        compress = self.archive_source[start:end]

        self.assertIn("#if defined(__PS3__)", compress)
        ps3_return = compress.index("return true;")
        desktop_branch = compress.index("#else", ps3_return)
        desktop_allocation = compress.index("tempbuf_len = (length >> 6)", desktop_branch)
        self.assertLess(ps3_return, desktop_branch)
        self.assertLess(desktop_branch, desktop_allocation)
        self.assertIn("if (FileHeader[0] != 'C'", self.archive_source)
        self.assertIn("g_lz77.Decompress(pos, length - 8, tempbuf, &iCSVGLength)", self.archive_source)
        self.assertIn("pos = buffer;", self.archive_source)

    def test_shader_cache_is_selected_for_the_running_title(self):
        self.assertIn('ri.Cvar_VariableIntegerValue("com_target_game")', self.shader_source)
        self.assertIn('return "scripts/ps3_shader_cache.bin";', self.shader_source)
        self.assertIn('return "scripts/ps3_shader_cache_mainta.bin";', self.shader_source)
        self.assertIn('return "scripts/ps3_shader_cache_maintt.bin";', self.shader_source)
        self.assertNotIn('#define PS3_SHADER_CACHE_PATH', self.shader_source)
        self.assertIn("memchr(expectedName, '\\0', sourceRecordSize)", self.shader_source)
        self.assertNotIn("memchr(expectedName, '\\\\0', sourceRecordSize)", self.shader_source)

    def test_per_tag_snapshots_cover_cgame_map_initialization(self):
        self.assertIn("void PS3_ZoneDumpTags(const char *where);", self.ps3_step)
        self.assertIn("PS3_BootTraceFastf( \"ZONE TAGSUMMARY[%s]", self.memory)
        for stage in (
            "CGINIT enter",
            "CGINIT after-shutdown",
            "CGINIT before-cg-init",
            "CGINIT after-cg-init",
            "CGINIT after-tiki-finish",
            "CGINIT after-end-registration",
        ):
            self.assertIn(f'PS3_ZONEDUMP("{stage}")', self.cgame)

    def test_workflow_runs_renderer_memory_regressions(self):
        self.assertIn("Test PS3 renderer cache and zone memory diagnostics", self.workflow)
        self.assertIn("test_ps3_renderer_memory.py", self.workflow)


    def test_ps3_bsp_path_streams_vertex_and_index_lumps_by_surface(self):
        start = self.world_bsp.index(
            "numDrawSurfaces = R_LoadLump(h, Q_GetLumpByVersion(&header, LUMP_SURFACES)"
        )
        ps3_start = self.world_bsp.index("#if defined(__PS3__)", start)
        fallback_start = self.world_bsp.index("#else", ps3_start)
        fallback_end = self.world_bsp.index("#endif", fallback_start)
        ps3_branch = self.world_bsp[ps3_start:fallback_start]
        fallback_branch = self.world_bsp[fallback_start:fallback_end]

        self.assertIn("R_LoadSurfacesPS3(h, name, length", ps3_branch)
        self.assertNotIn("LUMP_DRAWVERTS), &lump2", ps3_branch)
        self.assertNotIn("LUMP_DRAWINDEXES), &lump3", ps3_branch)
        self.assertIn("LUMP_DRAWVERTS), &lump2", fallback_branch)
        self.assertIn("LUMP_DRAWINDEXES), &lump3", fallback_branch)

    def test_ps3_bsp_stream_scratch_is_limited_to_the_largest_surface(self):
        start = self.world_bsp.index("static qboolean R_LoadSurfacesPS3(")
        end = self.world_bsp.index("/*\n=================\nR_LoadSubmodels", start)
        streaming = self.world_bsp[start:end]

        self.assertIn("R_PS3ReadBspRange", streaming)
        self.assertIn("maxVertices * sizeof(drawVert_t)", streaming)
        self.assertIn("maxIndexes * sizeof(int)", streaming)
        self.assertIn("backwardVertexSeeks", streaming)
        self.assertIn("backwardIndexSeeks", streaming)
        self.assertIn("MAX_PATCH_SIZE", self.world_bsp)
        self.assertIn("count > totalVertices - first", self.world_bsp)
        self.assertIn("count > totalIndexes - first", self.world_bsp)


if __name__ == "__main__":
    unittest.main()
