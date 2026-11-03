import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PS3RendererPerformanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tr_cmds = (ROOT / "code/renderergl1/tr_cmds.c").read_text()
        cls.tr_backend = (ROOT / "code/renderergl1/tr_backend.c").read_text()
        cls.tr_shade = (ROOT / "code/renderergl1/tr_shade.c").read_text()
        cls.tr_shade_calc = (ROOT / "code/renderergl1/tr_shade_calc.c").read_text()
        cls.tr_sun_flare = (ROOT / "code/renderergl1/tr_sun_flare.cpp").read_text()
        cls.ps3gl_draw = (ROOT / "code/gl/ps3gl_draw.c").read_text()
        cls.ps3gl_textures = (ROOT / "code/gl/ps3gl_textures.c").read_text()
        cls.ps3gl_shaders = (ROOT / "code/gl/ps3gl_shaders.c").read_text()
        cls.ps3_prof = (ROOT / "code/sys/ps3_prof.c").read_text()
        cls.ps3_platform = (ROOT / "code/sys/ps3_platform.h").read_text()
        cls.workflow = (ROOT / ".github/workflows/ps3-build.yml").read_text()

    def test_renderer_stays_synchronous_until_shared_scratch_is_isolated(self):
        # Terrain tessellation, TIKI bones/morphs, static-model descriptors and
        # swipe surfaces still use shared transient storage. Do not dispatch
        # the back end asynchronously until those allocations are per-frame.
        self.assertIn("RB_ExecuteRenderCommands( cmdList->cmds );", self.tr_cmds)
        self.assertNotIn("GLimp_SpawnRenderThread", self.tr_cmds)
        self.assertNotIn("GLimp_WakeRenderer", self.tr_cmds)

    def test_backend_view_reads_use_the_command_snapshot(self):
        self.assertIn("backEnd.refdef.vieworg", self.tr_shade_calc)
        self.assertIn("backEnd.refdef.viewaxis", self.tr_shade_calc)
        self.assertIn("backEnd.refdef.", self.tr_sun_flare)
        self.assertIn("backEnd.viewParms.", self.tr_sun_flare)
        self.assertNotIn("tr.refdef.vieworg", self.tr_sun_flare)
        self.assertNotIn("tr.viewParms.projectionMatrix", self.tr_sun_flare)

    def test_additive_multitexture_uses_two_depth_coherent_passes(self):
        self.assertIn("pStage->multitextureEnv == GL_ADD", self.tr_shade)
        self.assertIn("GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE", self.tr_shade)
        self.assertIn("GLS_DEPTHFUNC_EQUAL", self.tr_shade)
        self.assertIn("qglDisable(GL_TEXTURE_2D)", self.tr_shade)
        self.assertIn("input->svars.texcoords[0]", self.tr_shade)
        self.assertIn("input->svars.texcoords[1]", self.tr_shade)

    def test_single_texture_blend_environment_selects_blend_shader(self):
        self.assertIn("case GL_BLEND:          mode = PS3GL_TENV_BLEND; break;", self.ps3gl_textures)
        self.assertIn("load_fp(&ps3gl.shaders[PS3GL_TENV_BLEND]", self.ps3gl_shaders)
        self.assertIn("return ps3gl.tmu[0].texenv;", self.ps3gl_shaders)

    def test_vertex_copy_fallback_and_direct_xdr_binding_are_counted(self):
        self.assertIn("ps3gl_stats.direct_draws++", self.ps3gl_draw)
        self.assertIn("ps3gl_stats.copy_draws++", self.ps3gl_draw)
        self.assertIn("ps3gl_stats.copy_verts", self.ps3gl_draw)
        self.assertIn("ps3gl_stats.copy_bytes", self.ps3gl_draw)
        self.assertIn("PS3_PROF_GLCOPY", self.ps3gl_draw)

    def test_renderer_profiler_reports_fps_and_backend_phases(self):
        self.assertIn('"[fps] avg=%.1f', self.ps3_prof)
        for field in ("PS3_PROF_BACKEND", "PS3_PROF_GLCOPY", "PS3_PROF_SHADE",
                      "PS3_PROF_SURF", "PS3_PROF_ANIM", "PS3_PROF_TCOORDS"):
            self.assertIn(field, self.ps3_prof)
        self.assertIn("void PS3_ProfReportFrame(void);", self.ps3_platform)
        self.assertIn("PS3_ProfReportFrame();", self.tr_cmds)

    def test_three_ps3_variants_keep_incremental_ninja_build_trees(self):
        # All three build directories are restored separately; CMake/Ninja
        # then reuses objects and recompiles only dependencies of changed files.
        for appid in ("OMOHAA020", "OMOHAB020", "OMOHAS020"):
            self.assertIn(f"build-ps3-{appid}", self.workflow)
        self.assertIn("Restore incremental PS3 build tree", self.workflow)
        self.assertIn(".ps3-source-hashes.json", self.workflow)
        self.assertIn("Restore PS3 compiler cache", self.workflow)
        self.assertIn("CMAKE_C_COMPILER_LAUNCHER=ccache", self.workflow)
        self.assertIn("CMAKE_CXX_COMPILER_LAUNCHER=ccache", self.workflow)
        self.assertIn('cmake --build "$build_dir" --parallel 2', self.workflow)
        self.assertNotIn("--clean-first", self.workflow)


if __name__ == "__main__":
    unittest.main()
