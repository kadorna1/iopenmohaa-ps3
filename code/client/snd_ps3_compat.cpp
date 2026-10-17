/* PS3 compatibility layer for high-level sound/music APIs.
 * The low-level PS3 driver is intentionally silent at this port stage.
 * Keep the engine-facing ABI complete without pulling in OpenAL. */
#include "snd_local_new.h"
#include "snd_public.h"

extern "C" {

qboolean S_IsSoundPlaying(int channel_number, const char *sfxName)
{
    (void)channel_number;
    (void)sfxName;
    return qfalse;
}

void S_FadeSound(float fTime) { (void)fTime; }
void MUSIC_NewSoundtrack(const char *name) { (void)name; }
void MUSIC_UpdateMood(int current, int fallback) { (void)current; (void)fallback; }
void MUSIC_UpdateVolume(float volume, float fade_time) { (void)volume; (void)fade_time; }
const char *S_CurrentSoundtrack(void) { return ""; }
void S_TriggeredMusic_SetupHandle(const char *name, int loopCount, int offset, qboolean autostart)
{ (void)name; (void)loopCount; (void)offset; (void)autostart; }
void S_TriggeredMusic_PlayIntroMusic(void) {}
void S_TriggeredMusic_Stop(void) {}
void S_StopMovieAudio(void) {}
void S_SetupMovieAudio(const char *movieName) { (void)movieName; }
int S_CurrentMoviePosition(void) { return 0; }
void S_PrintInfo(void) {}
void S_DumpInfo(void) {}
qboolean S_NeedFullRestart(void) { return qfalse; }
void S_SaveData(soundsystemsavegame_t *save) { (void)save; }
void S_LoadData(soundsystemsavegame_t *save) { (void)save; }
void S_ReLoad(soundsystemsavegame_t *save) { (void)save; }

}
