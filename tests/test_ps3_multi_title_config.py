import pathlib
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
        cls.workflow = (ROOT / ".github/workflows/ps3-build.yml").read_text()

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

    def test_expansion_executables_select_their_native_target(self):
        self.assertIn('Cvar_Get("com_target_game", PS3_DEFAULT_TARGET_GAME,', self.common)
        self.assertIn("PS3_DEFAULT_TARGET_GAME", self.cmake)
        self.assertIn("TITLE_GAME_IDS=(0 1 2)", self.workflow)
        self.assertIn("TITLE_APPIDS=(OMOHAA020 OMOHAB020 OMOHAS020)", self.workflow)

    def test_workflow_builds_packages_and_saves_incremental_manifests(self):
        for appid in ("OMOHAA020", "OMOHAB020", "OMOHAS020"):
            self.assertIn(f"build-ps3-{appid}", self.workflow)
            self.assertIn(f"iOpenMOHAA-PS3-{appid}", self.workflow)
        self.assertIn("--appid", self.workflow)
        self.assertIn("--extra-usrdir assets/ps3_usrdir", self.workflow)
        self.assertIn("Save source hashes for the next incremental build", self.workflow)
        self.assertIn("ps3-build-v3-", self.workflow)
        self.assertIn("--recurse-submodules", self.workflow)
        self.assertIn("changed_union", self.workflow)
        self.assertNotIn("Link EBOOT.BIN with PSL1GHT", self.workflow)
        self.assertNotIn("cmake --build build-ps3 --target ps3_self", self.workflow)


if __name__ == "__main__":
    unittest.main()
