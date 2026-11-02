import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class Ps3ExpansionLoadingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.commands = (ROOT / "code/cgame/cg_commands.cpp").read_text()
        cls.mp3 = (ROOT / "code/client/snd_codec_mp3.c").read_text()
        cls.ps3_audio = (ROOT / "code/client/snd_ps3audio_new.cpp").read_text()

    def test_ps3_defers_eager_wav_and_mp3_alias_cache_by_default(self):
        start = self.commands.index("void CacheResource(const char *stuff)\n{")
        end = self.commands.index("//===============\n// Cache", start)
        cache_resource = self.commands[start:end]
        self.assertIn("#if defined(__PS3__)", cache_resource)
        self.assertIn('cgi.Cvar_Get(\n                "ps3_defer_sound_precache", "1", CVAR_ARCHIVE', cache_resource)
        self.assertIn('strstr(real_stuff.c_str(), ".wav")', cache_resource)
        self.assertIn('strstr(real_stuff.c_str(), ".mp3")', cache_resource)
        self.assertIn("set ps3_defer_sound_precache 0", cache_resource)
        # Real playback still registers the chosen alias on demand.
        self.assertIn("cgi.S_RegisterSound(name, soundAlias->streamed)", self.commands)

    def test_mp3_stall_log_includes_file_and_decoder_state(self):
        self.assertIn("minimp3 input buffer full", self.mp3)
        self.assertIn("file_pos=%d", self.mp3)
        self.assertIn("frames=%u skipped=%u errors=%u freefmt=%d", self.mp3)
        self.assertIn("decoder.header[0]", self.mp3)
        self.assertIn("encbuf[tail + 3]", self.mp3)
        self.assertIn("FS_FTell(stream->file)", self.mp3)

    def test_streaming_driver_keeps_decoder_reads_on_main_update_path(self):
        # The MP3 decoder is refilled into a ring; the output thread mixes the
        # ring rather than owning or concurrently reading the file handle.
        self.assertIn("S_CodecReadStream(d, request, raw)", self.ps3_audio)
        self.assertIn("ps3a_stream_refill(i)", self.ps3_audio)


if __name__ == "__main__":
    unittest.main()
