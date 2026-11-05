#!/usr/bin/env python3
"""Regression checks for the PS3 level-scoped decoded speech cache."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PS3SpeechCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.audio = (ROOT / "code/client/snd_ps3audio_new.cpp").read_text(encoding="utf-8")
        cls.local = (ROOT / "code/client/snd_local_new.h").read_text(encoding="utf-8")
        cls.dma = (ROOT / "code/client/snd_dma_new.cpp").read_text(encoding="utf-8")
        cls.cgame = (ROOT / "code/cgame/cg_main.c").read_text(encoding="utf-8")
        cls.public = (ROOT / "code/cgame/cg_public.h").read_text(encoding="utf-8")
        cls.client = (ROOT / "code/client/cl_cgame.cpp").read_text(encoding="utf-8")

    def test_cache_has_bounded_level_scoped_storage(self):
        self.assertIn("PS3A_SPEECH_CACHE_DEFAULT_MB 8", self.audio)
        self.assertIn("PS3A_SPEECH_CACHE_MAX_MB 16", self.audio)
        self.assertIn('Cvar_Get("s_ps3_speech_cache_mb", "8", CVAR_ARCHIVE)', self.audio)
        self.assertIn("ps3a_speech_cache_begin_internal", self.audio)
        self.assertIn("ps3a_speech_cache_clear_internal", self.audio)
        self.assertIn("speech cache ready level=%s clips=%d used=%u budget=%u", self.audio)

    def test_only_streamed_speech_aliases_are_cached(self):
        self.assertIn('#include "../qcommon/alias.h"', self.cgame)
        for channel in ("voice", "dialog", "dialog_secondary"):
            self.assertIn(f'cgi.S_ChannelNameToNum("{channel}")', self.cgame)
        self.assertIn("node->forcesubtitle", self.cgame)
        self.assertIn("!node->streamed || !node->real_name[0]", self.cgame)
        self.assertIn("CG_BuildLevelSpeechCache();", self.cgame)

    def test_entire_clip_is_decoded_and_deduplicated(self):
        self.assertIn("PS3A_SPEECH_CACHE_MAX_ENTRIES 512", self.audio)
        self.assertIn("S_CodecReadStream(decoder,", self.audio)
        self.assertIn("totalFrames += (uint32_t)frames;", self.audio)
        self.assertIn("Q_stricmp(g_speech_entries[i].filename, filename)", self.audio)
        self.assertIn("Partial clips are never exposed", self.audio)

    def test_cached_stream_uses_existing_ring_and_preserves_mono(self):
        self.assertIn("s->speechPcm = fromSpeechCache ? speech.pcm : NULL;", self.audio)
        self.assertIn("if ((!d && !speechPcm) || s->paused || s->eof)", self.audio)
        self.assertIn("speechPcm[sourceFrame * (uint64_t)chans]", self.audio)
        self.assertIn("const short right = chans == 2", self.audio)
        self.assertIn("g_stream_ring[slot][dstFrame * 2u + 1u] = right;", self.audio)
        self.assertIn("speech stream cache hit", self.audio)

    def test_non_cached_and_offset_streams_keep_decoder_fallback(self):
        self.assertIn("if (offsetFrames == 0)", self.audio)
        self.assertIn("ps3a_speech_cache_take(filename, &speech)", self.audio)
        self.assertIn("d=ps3a_stream_open_decoder(filename,&rate,&width,&chans,offsetFrames);", self.audio)
        self.assertIn("if(!d && !fromSpeechCache)", self.audio)

    def test_level_transition_stops_streams_before_freeing_pool(self):
        begin = self.dma.index("void S_BeginRegistration()")
        end = self.dma.index("void S_DefaultSound", begin)
        registration = self.dma[begin:end]
        self.assertLess(registration.index("S_StopAllSounds(true);"),
                        registration.index("S_PS3AUDIO_ClearSpeechCache();"))
        self.assertIn("S_PS3AUDIO_ClearSpeechCache();", self.audio)
        self.assertIn("void          S_PS3AUDIO_ClearSpeechCache(void);", self.local)

    def test_cgame_callbacks_are_ps3_only(self):
        for field in ("S_BeginSpeechCache", "S_PrecacheSpeech", "S_EndSpeechCache"):
            self.assertIn(field, self.public)
            self.assertIn(field, self.client)
        self.assertIn("#if defined(__PS3__)", self.client)


if __name__ == "__main__":
    unittest.main()
