import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PS3RendererSMPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tr_init = (ROOT / "code/renderergl1/tr_init.c").read_text()
        cls.tr_local = (ROOT / "code/renderergl1/tr_local.h").read_text()
        cls.tr_cmds = (ROOT / "code/renderergl1/tr_cmds.c").read_text()
        cls.tr_backend = (ROOT / "code/renderergl1/tr_backend.c").read_text()
        cls.tr_scene = (ROOT / "code/renderergl1/tr_scene.c").read_text()
        cls.tr_shade = (ROOT / "code/renderergl1/tr_shade.c").read_text()
        cls.tr_shade_calc = (ROOT / "code/renderergl1/tr_shade_calc.c").read_text()
        cls.tr_sun_flare = (ROOT / "code/renderergl1/tr_sun_flare.cpp").read_text()
        cls.ps3gl_draw = (ROOT / "code/gl/ps3gl_draw.c").read_text()
        cls.ps3gl_textures = (ROOT / "code/gl/ps3gl_textures.c").read_text()
        cls.ps3_prof = (ROOT / "code/sys/ps3_prof.c").read_text()
        cls.ps3_platform = (ROOT / "code/sys/ps3_platform.h").read_text()

    def test_ps3_allocates_two_independent_renderer_frame_data_blocks(self):
        self.assertIn('ri.Cvar_Get( "r_smp", "1"', self.tr_init)
        self.assertIn("frameDataSize * (r_smp->integer ? 2 : 1)", self.tr_init)
        self.assertIn("secondBackEndData = (backEndData_t *)secondPtr", self.tr_init)
        self.assertIn("frontEndData = backEndData", self.tr_init)
        self.assertIn("R_InitCommandBuffers(secondBackEndData, r_smp->integer ? qtrue : qfalse)", self.tr_init)

    def test_scene_building_uses_frontend_owned_storage(self):
        self.assertIn("cmdList = &frontEndData->commands;", self.tr_cmds)
        self.assertIn("poly = &frontEndData->polys", self.tr_scene)
        self.assertIn("frontEndData->entities", self.tr_scene)
        self.assertIn("frontEndData->drawSurfs", self.tr_scene)
        self.assertIn("frontEndData->staticModelData", self.tr_scene)

    def test_ps3_worker_dispatch_waits_before_reusing_each_frame_buffer(self):
        self.assertIn("GLimp_SpawnRenderThread(RB_RenderThread)", self.tr_cmds)
        self.assertIn("GLimp_WakeRenderer(&s_renderJob)", self.tr_cmds)
        self.assertIn("GLimp_FrontEndSleep()", self.tr_cmds)
        self.assertIn("s_frontFrame ^= 1u", self.tr_cmds)
        self.assertIn("R_ShutdownCommandBuffers()", self.tr_init)
        self.assertIn("GLimp_ShutdownRenderThread()", self.tr_cmds)

    def test_backend_consumes_frame_local_view_and_dynamic_light_state(self):
        for field in ("frameCount", "smpFrame", "portalRendered", "skyRendered", "farclip"):
            self.assertIn(f"cmd->{field}", self.tr_cmds)
            self.assertIn(f"backEnd.{field} = cmd->{field}", self.tr_backend)
        self.assertIn("backEnd.refdef = cmd->refdef", self.tr_backend)
        self.assertIn("backEnd.viewParms = cmd->viewParms", self.tr_backend)
        self.assertIn("backEnd.refdef.vieworg", self.tr_shade_calc)
        self.assertIn("backEnd.refdef.viewaxis", self.tr_shade_calc)
        self.assertIn("backEnd.refdef.vieworg", self.tr_sun_flare)
        self.assertNotIn("tr.refdef.vieworg", self.tr_sun_flare)

    def test_additive_multitexture_uses_two_single_texture_passes(self):
        self.assertIn("pStage->multitextureEnv == GL_ADD", self.tr_shade)
        self.assertIn("GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE", self.tr_shade)
        self.assertIn("qglDisable(GL_TEXTURE_2D)", self.tr_shade)
        self.assertIn("case GL_BLEND:          mode = PS3GL_TENV_BLEND; break;", self.ps3gl_textures)

    def test_vertex_copy_and_renderer_phase_profiler_are_reported(self):
        self.assertIn("ps3gl_stats.direct_draws++", self.ps3gl_draw)
        self.assertIn("ps3gl_stats.copy_draws++", self.ps3gl_draw)
        self.assertIn("ps3gl_stats.copy_bytes", self.ps3gl_draw)
        self.assertIn('"[fps] avg=%.1f', self.ps3_prof)
        self.assertIn("PS3_PROF_GLCOPY", self.ps3_prof)
        self.assertIn("PS3_ProfReportFrame(void)", self.ps3_platform)


if __name__ == "__main__":
    unittest.main()
