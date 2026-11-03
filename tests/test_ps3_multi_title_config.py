import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class Ps3MultiTitleConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = (ROOT / "cmake/platforms/ps3.cmake").read_text()
        cls.header = (ROOT / "code/sys/ps3_platform.h").read_text()
        cls.ps3_sys = (ROOT / "code/sys/ps3_sys.c").read_text()
        cls.sys_main = (ROOT / "code/sys/sys_main.c").read_text()
        cls.common = (ROOT / "code/qcommon/common.c").read_text()
        cls.q_shared = (ROOT / "code/qcommon/q_shared.h").read_text()
        cls.files = (ROOT / "code/qcommon/files.cpp").read_text()
        cls.workflow = (ROOT / ".github/workflows/ps3-build.yml").read_text()
        cls.basegame = (ROOT / "cmake/basegame.cmake").read_text()
        cls.ps3_platform = (ROOT / "cmake/platforms/ps3.cmake").read_text()
        cls.ps3_input = (ROOT / "code/input/ps3_input.c").read_text()
        cls.client_input = (ROOT / "code/client/cl_input.cpp").read_text()
        cls.client_keys = (ROOT / "code/client/cl_keys.cpp").read_text()
        cls.client_parse = (ROOT / "code/client/cl_parse.cpp").read_text()
        cls.cgame_drawtools = (ROOT / "code/cgame/cg_drawtools.cpp").read_text()
        cls.cgame_public = (ROOT / "code/cgame/cg_public.h").read_text()
        cls.cgame_main = (ROOT / "code/cgame/cg_main.c").read_text()
        cls.actor = (ROOT / "code/fgame/actor.cpp").read_text()
        cls.sentient = (ROOT / "code/fgame/sentient.h").read_text()
        cls.stage_script = ROOT / "tools/stage_ps3_title_usrdir.sh"

    def test_title_ids_are_valid_and_distinct(self):
        ids = ("OMOHAA020", "OMOHAB020", "OMOHAS020")
        self.assertEqual(len(set(ids)), 3)
        for appid in ids:
            self.assertEqual(len(appid), 9)
            self.assertTrue(appid.isascii() and appid.isalnum() and appid.isupper())
            self.assertIn(appid, self.workflow)
        self.assertIn("OPENMOHAA_PS3_DATA_APPID", self.cmake)
        self.assertIn("OPENMOHAA_PS3_TARGET_GAME", self.cmake)

    def test_private_state_and_shared_install_path_are_separate(self):
        self.assertIn('#define PS3_USRDIR      "/dev_hdd0/game/" PS3_APPID "/USRDIR"', self.header)
        self.assertIn('#define PS3_DATA_USRDIR "/dev_hdd0/game/" PS3_DATA_APPID "/USRDIR"', self.header)
        for fn in ("Sys_DefaultHomePath", "Sys_DefaultHomeConfigPath",
                   "Sys_DefaultHomeDataPath", "Sys_DefaultHomeStatePath"):
            self.assertIn(f"{fn}(void) {{ return PS3_USRDIR; }}", self.ps3_sys)
        self.assertIn("Sys_SetDefaultInstallPath(PS3_DATA_USRDIR);", self.ps3_sys)
        self.assertIn("Sys_SetDefaultInstallPath( PS3_DATA_USRDIR );", self.sys_main)
        self.assertIn('PS3_Logf("OpenMoHAA PS3 platform init (app USRDIR=%s, shared data=%s)"', self.ps3_sys)
        # The filesystem mounts the private home paths and shared base path.
        self.assertIn("FS_AddPathVar( fs_homeconfigpath )", self.files)
        self.assertIn("FS_AddPathVar( fs_homedatapath )", self.files)
        self.assertIn("FS_AddPathVar( fs_homestatepath )", self.files)
        self.assertIn("FS_AddPathVar( fs_basepath )", self.files)
        self.assertIn("FS_AddGameDirectory(pathVar->string, dir)", self.files)

    def test_expansion_executables_select_their_native_target(self):
        self.assertIn('Cvar_Get("com_target_game", PS3_DEFAULT_TARGET_GAME,', self.common)
        self.assertIn("PS3_DEFAULT_TARGET_GAME", self.cmake)
        self.assertIn("TITLE_GAME_IDS=(0 2 1)", self.workflow)
        # Every title build compiles the same PS3 input, game and CGame modules.
        self.assertIn("input/ps3_input.c", self.ps3_platform)
        self.assertIn("set(PS3_GAME_OBJ_TARGET", self.basegame)
        self.assertIn("set(PS3_CGAME_OBJ_TARGET", self.basegame)
        self.assertIn("target_sources(", self.basegame)
        self.assertIn('Cvar_Get("ps3_aim_assist", "1", CVAR_ARCHIVE);', self.ps3_input)
        self.assertIn('Cvar_Get("ps3_aim_assist_strength", "1.0", CVAR_ARCHIVE);', self.ps3_input)
        self.assertIn("cge->CG_AimAssist", self.client_input)
        self.assertIn("cge.CG_AimAssist                 = CG_AimAssist;", self.cgame_main)
        self.assertIn("TEAM_AMERICAN", self.sentient)
        self.assertIn("TEAM_GERMAN", self.sentient)
        self.assertIn("edict->s.eFlags |= EF_ALLIES;", self.actor)
        self.assertIn("edict->s.eFlags |= EF_AXIS;", self.actor)
        # OpenMoHAA's native enum uses MOHTA for Spearhead and MOHTT for Breakthrough.
        self.assertIn('#define PRODUCT_EXTENSION_MOHTA\t\t\t"Spearhead"', self.q_shared)
        self.assertIn('#define GAME_EXTENSION_MOHTA\t\t\t"mainta"', self.q_shared)
        self.assertIn('#define PRODUCT_EXTENSION_MOHTT\t\t\t"Breakthrough"', self.q_shared)
        self.assertIn('#define GAME_EXTENSION_BASE_MOHTT\t\t"maintt"', self.q_shared)
        self.assertIn("TITLE_APPIDS=(OMOHAA020 OMOHAB020 OMOHAS020)", self.workflow)

    def test_aim_assist_defaults_are_installed_in_each_title_directory(self):
        title_game_dirs = {
            "OMOHAA020": "main",
            "OMOHAB020": "maintt",
            "OMOHAS020": "mainta",
        }
        expected = {
            'seta ps3_aim_assist "1"',
            'seta ps3_aim_assist_strength "1.0"',
        }
        for appid, game_dir in title_game_dirs.items():
            config = ROOT / "assets/ps3_usrdir" / game_dir / "autoexec.cfg"
            self.assertTrue(config.is_file(), f"{appid} is missing {game_dir}/autoexec.cfg")
            lines = set(config.read_text().splitlines())
            self.assertTrue(expected.issubset(lines), f"{appid} is missing aim-assist defaults")


    def test_aim_assist_uses_r1_and_the_game_saved_centerprint_path(self):
        self.assertIn("if (key == K_JOY6 && down", self.client_keys)
        self.assertNotIn("if (key == K_JOY7 && down", self.client_keys)
        self.assertIn('UI_UpdateCenterPrint(enabled ? "AIM ASSIST ON" : "AIM ASSIST OFF", 1.0f);', self.client_input)
        self.assertIn("UI_UpdateCenterPrint(string, 1.0);", self.client_parse)
        self.assertNotIn("CG_AimAssistNotify", self.client_input)
        self.assertNotIn("CG_AimAssistNotify", self.cgame_public)
        self.assertNotIn("CG_DrawAimAssistStatus", self.cgame_drawtools)

    def test_workflow_builds_packages_and_saves_incremental_manifests(self):
        for appid in ("OMOHAA020", "OMOHAB020", "OMOHAS020"):
            self.assertIn(f"build-ps3-{appid}", self.workflow)
        self.assertIn('--name "iOpenMOHAA-PS3-$appid"', self.workflow)
        self.assertIn("--appid", self.workflow)
        self.assertIn("tools/stage_ps3_title_usrdir.sh", self.workflow)
        self.assertIn('--extra-usrdir "$stage_usrdir"', self.workflow)
        self.assertIn("assets/ps3_usrdir", self.workflow)
        self.assertIn("Save source hashes for the next incremental build", self.workflow)
        self.assertIn("ps3-build-v3-", self.workflow)
        self.assertIn("--recurse-submodules", self.workflow)
        self.assertIn("changed_union", self.workflow)
        self.assertNotIn("Link EBOOT.BIN with PSL1GHT", self.workflow)
        self.assertNotIn("cmake --build build-ps3 --target ps3_self", self.workflow)
        self.assertIn("Verify all three installable PKGs", self.workflow)
        self.assertIn("source revision %s", self.workflow)
        expected_artifacts = {
            "Allied Assault": "OMOHAA020",
            "Breakthrough": "OMOHAB020",
            "Spearhead": "OMOHAS020",
        }
        for product, appid in expected_artifacts.items():
            self.assertIn(f"Upload {product} PKG", self.workflow)
            self.assertIn(f"dist/iOpenMOHAA-PS3-{appid}.pkg", self.workflow)

    def test_expansion_usrdir_staging_keeps_titles_separate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            source = root / "source"
            (source / "main").mkdir(parents=True)
            config_text = (
                'seta ps3_aim_assist "1"\n'
                'seta ps3_aim_assist_strength "1.0"\n'
            )
            (source / "main" / "autoexec.cfg").write_text(config_text)
            (source / "mainta").mkdir()
            (source / "mainta" / "autoexec.cfg").write_text(config_text)
            (source / "mainta" / "spearhead.pk3").write_bytes(b"spearhead")
            (source / "maintt").mkdir()
            (source / "maintt" / "autoexec.cfg").write_text(config_text)
            (source / "maintt" / "breakthrough.pk3").write_bytes(b"breakthrough")

            staged = {}
            for appid in ("OMOHAA020", "OMOHAB020", "OMOHAS020"):
                destination = root / appid
                subprocess.run(
                    ["bash", str(self.stage_script), appid, str(source), str(destination)],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                staged[appid] = destination

            # Common runtime files are staged into every title.
            for destination in staged.values():
                self.assertTrue((destination / "main" / "autoexec.cfg").is_file())

            # Only the selected expansion's tree is copied to each expansion PKG.
            self.assertFalse((staged["OMOHAA020"] / "mainta").exists())
            self.assertFalse((staged["OMOHAA020"] / "maintt").exists())
            self.assertTrue((staged["OMOHAB020"] / "maintt" / "breakthrough.pk3").is_file())
            self.assertTrue((staged["OMOHAB020"] / "maintt" / "autoexec.cfg").is_file())
            self.assertFalse((staged["OMOHAB020"] / "mainta").exists())
            self.assertTrue((staged["OMOHAS020"] / "mainta" / "spearhead.pk3").is_file())
            self.assertTrue((staged["OMOHAS020"] / "mainta" / "autoexec.cfg").is_file())
            self.assertFalse((staged["OMOHAS020"] / "maintt").exists())
            for appid, game_dir in (("OMOHAB020", "maintt"), ("OMOHAS020", "mainta")):
                config = (staged[appid] / game_dir / "autoexec.cfg").read_text()
                self.assertIn('seta ps3_aim_assist "1"', config)
                self.assertIn('seta ps3_aim_assist_strength "1.0"', config)


if __name__ == "__main__":
    unittest.main()
