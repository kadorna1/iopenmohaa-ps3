/*
===========================================================================
PS3 null sound driver.

The first PS3 port stage keeps the engine silent until a native CELL audio
backend is linked.  This preserves the engine sound-driver ABI without
pulling desktop OpenAL headers or libraries into the PSL1GHT build.
===========================================================================
*/

#include "snd_local.h"

/* s_show_sounds is only defined by the OpenAL driver (snd_openal_new.cpp),
 * which the PS3 build excludes, but snd_dma_new.cpp assigns and reads it.
 * Weak, so a strong definition elsewhere in the tree still wins. */
cvar_t *s_show_sounds __attribute__((weak));

extern "C" {

qboolean S_PS3NULL_Init()
{
    return qfalse;
}

void S_PS3NULL_Shutdown() {}
void S_PS3NULL_StartSound(const vec3_t, int, int, sfxHandle_t, float, float, float, float, qboolean) {}
void S_PS3NULL_AddLoopingSound(const vec3_t, const vec3_t, sfxHandle_t, float, float, float, float, int) {}
void S_PS3NULL_ClearLoopingSounds() {}
void S_PS3NULL_StopSound(int, int) {}
void S_PS3NULL_StopAllSounds(qboolean) {}
void S_PS3NULL_Respatialize(int, const vec3_t, const vec3_t[3]) {}
void S_PS3NULL_SetReverb(int, float) {}
void S_PS3NULL_Update() {}

const char *S_PS3NULL_GetMusicFilename()
{
    return "";
}

int S_PS3NULL_GetMusicLoopCount()
{
    return 0;
}

unsigned int S_PS3NULL_GetMusicOffset()
{
    return 0;
}

}
