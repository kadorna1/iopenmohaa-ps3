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
        # OpenMoHAA's native enum uses MOHTA for Spearhead and MOHTT for Breakthrough.
        self.assertIn('#define PRODUCT_EXTENSION_MOHTA\t\t\t"Spearhead"', self.q_shared)
        self.assertIn('#define GAME_EXTENSION_MOHTA\t\t\t"mainta"', self.q_shared)
        self.assertIn('#define PRODUCT_EXTENSION_MOHTT\t\t\t"Breakthrough"', self.q_shared)
        self.assertIn('#define GAME_EXTENSION_BASE_MOHTT\t\t"maintt"', self.q_shared)
        self.assertIn("TITLE_APPIDS=(OMOHAA020 OMOHAB020 OMOHAS020)", self.workflow)

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

    def test_expansion_usrdir_staging_keeps_titles_separate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            source = root / "source"
            (source / "main").mkdir(parents=True)
            (source / "main" / "autoexec.cfg").write_text("set test 1\n")
            (source / "mainta").mkdir()
            (source / "mainta" / "spearhead.pk3").write_bytes(b"spearhead")
            (source / "maintt").mkdir()
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
            self.assertFalse((staged["OMOHAB020"] / "mainta").exists())
            self.assertTrue((staged["OMOHAS020"] / "mainta" / "spearhead.pk3").is_file())
            self.assertFalse((staged["OMOHAS020"] / "maintt").exists())


if __name__ == "__main__":
    unittest.main()
