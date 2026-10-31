/* PS3 compatibility layer for the higher-level soundtrack and cinematic APIs.
 * The native libaudio mixer consumes decoded stream rings prepared by the
 * game's registered WAV/MP3/Vorbis/Opus codecs. */
#include "snd_local_new.h"
#include "snd_public.h"
#include "client.h"
#include "../qcommon/alias.h"

#include <stdlib.h>
#include <string.h>

#define PS3_MUSIC_MAX_SONGS 16

static song_t ps3_music_songs[PS3_MUSIC_MAX_SONGS];
static int ps3_music_num_songs;
static int ps3_music_song = -1;
static int ps3_music_last_failed = -1;
static int ps3_music_current_mood = mood_none;
static int ps3_music_fallback_mood = mood_none;
static qboolean ps3_music_active;
static qboolean ps3_music_loaded;
static char ps3_current_soundtrack[MAX_RES_NAME];
static float ps3_music_volume = 1.0f;
static float ps3_music_volume_start = 1.0f;
static float ps3_music_volume_target = 1.0f;
static float ps3_music_fade_seconds;
static int ps3_music_fade_start;

static int PS3_MusicFindSongByMood(int mood)
{
    int i;
    for (i = 0; i < ps3_music_num_songs; i++) {
        if (ps3_music_songs[i].mood_num == mood) return i;
    }
    return -1;
}

static void PS3_MusicApplyVolume(void)
{
    float base = s_musicVolume ? s_musicVolume->value : 0.9f;
    float ambient = s_ambientVolume ? s_ambientVolume->value : 1.0f;
    if (ps3_music_song >= 0 && ps3_music_song < ps3_music_num_songs) {
        S_PS3AUDIO_SetStreamVolume(0,
            ps3_music_songs[ps3_music_song].volume * ps3_music_volume * base * ambient);
    } else {
        S_PS3AUDIO_SetStreamVolume(0, 0.0f);
    }
}

static qboolean PS3_MusicStartSong(int songnum)
{
    song_t *song;
    int loops;
    if (songnum < 0 || songnum >= ps3_music_num_songs) return qfalse;
    song = &ps3_music_songs[songnum];
    loops = (song->flags & 1) ? 0 : 1; /* 0 is infinite in the stream mixer */
    if (!S_PS3AUDIO_StartStream(0, song->path, loops, 0,
            song->volume * ps3_music_volume * (s_musicVolume ? s_musicVolume->value : 0.9f)
                * (s_ambientVolume ? s_ambientVolume->value : 1.0f),
            1.0f, qtrue, NULL, 0, CHAN_LOCAL, 1.0f, 1.0f)) {
        Com_DPrintf("PS3 audio: couldn't start soundtrack song '%s'\n", song->path);
        ps3_music_last_failed = songnum;
        ps3_music_song = -1;
        return qfalse;
    }
    ps3_music_song = songnum;
    ps3_music_last_failed = -1;
    return qtrue;
}

static void PS3_MusicUpdateInternal(void)
{
    int selected;
    if (ps3_music_fade_seconds > 0.0f) {
        float elapsed = (float)(cls.realtime - ps3_music_fade_start) / 1000.0f;
        float t = elapsed / ps3_music_fade_seconds;
        if (t >= 1.0f) {
            t = 1.0f;
            ps3_music_fade_seconds = 0.0f;
        }
        if (t < 0.0f) t = 0.0f;
        ps3_music_volume = ps3_music_volume_start
            + (ps3_music_volume_target - ps3_music_volume_start) * t;
    }
    PS3_MusicApplyVolume();

    if (!ps3_music_active || !ps3_music_loaded || ps3_music_current_mood == mood_none) {
        if (S_PS3AUDIO_IsStreamActive(0)) S_PS3AUDIO_StopStream(0);
        ps3_music_song = -1;
        return;
    }

    selected = PS3_MusicFindSongByMood(ps3_music_current_mood);
    if (selected < 0 && ps3_music_fallback_mood != mood_none)
        selected = PS3_MusicFindSongByMood(ps3_music_fallback_mood);
    if (selected < 0) {
        if (S_PS3AUDIO_IsStreamActive(0)) S_PS3AUDIO_StopStream(0);
        ps3_music_song = -1;
        return;
    }

    if (ps3_music_song >= 0 && !S_PS3AUDIO_IsStreamActive(0)) {
        /* A finite song ended. Switch to the configured fallback mood. */
        if (!(ps3_music_songs[ps3_music_song].flags & 1)) {
            ps3_music_current_mood = ps3_music_fallback_mood;
            selected = PS3_MusicFindSongByMood(ps3_music_current_mood);
        }
        ps3_music_song = -1;
    }

    if (selected < 0) return;
    if (selected != ps3_music_song && selected != ps3_music_last_failed)
        PS3_MusicStartSong(selected);
}

extern "C" {

void PS3_CompatMusicUpdate(void)
{
    PS3_MusicUpdateInternal();
}


static void PS3_MusicCommand(void)
{
    if (Cmd_Argc() != 2) {
        Com_Printf("music <file>\n");
        return;
    }
    ps3_music_active = qfalse;
    ps3_music_song = -1;
    (void)S_PS3AUDIO_StartStream(0, Cmd_Argv(1), 0, 0,
        s_musicVolume ? s_musicVolume->value : 0.9f, 1.0f,
        qtrue, NULL, 0, CHAN_LOCAL, 1.0f, 1.0f);
}

static void PS3_StopMusicCommand(void)
{
    MUSIC_StopAllSongs();
    S_TriggeredMusic_Stop();
}

static void PS3_PlayMP3Command(void)
{
    if (Cmd_Argc() != 2) {
        Com_Printf("playmp3 <file.mp3>\n");
        return;
    }
    (void)S_PS3AUDIO_StartStream(1, Cmd_Argv(1), 1, 0, 1.0f, 1.0f,
        qtrue, NULL, 0, CHAN_LOCAL, 1.0f, 1.0f);
}

static void PS3_StopMP3Command(void)
{
    S_PS3AUDIO_StopStream(1);
}

void PS3_CompatAudioInit(void)
{
    static qboolean registered = qfalse;
    if (registered) return;
    Cmd_AddCommand("music", PS3_MusicCommand);
    Cmd_AddCommand("stopmusic", PS3_StopMusicCommand);
    Cmd_AddCommand("playmp3", PS3_PlayMP3Command);
    Cmd_AddCommand("stopmp3", PS3_StopMP3Command);
    Cmd_AddCommand("loadsoundtrack", S_loadsoundtrack);
    Cmd_AddCommand("playsong", S_PlaySong);
    Cmd_AddCommand("tmstart", S_TriggeredMusic_Start);
    Cmd_AddCommand("tmstartloop", S_TriggeredMusic_StartLoop);
    Cmd_AddCommand("tmstop", S_TriggeredMusic_Stop);
    Cmd_AddCommand("tmvolume", S_TriggeredMusic_Volume);
    registered = qtrue;
}


qboolean S_IsSoundPlaying(int channel_number, const char *sfxName)
{
    (void)channel_number;
    (void)sfxName;
    return S_PS3AUDIO_IsStreamActive(1);
}

void S_FadeSound(float fTime)
{
    (void)fTime;
}

qboolean MUSIC_LoadSoundtrackFile(const char *filename)
{
    char path[MAX_RES_NAME];
    char loadPath[MAX_RES_NAME];
    char token[MAX_STRING_CHARS];
    char args[4][MAX_RES_NAME];
    byte *data = NULL;
    char *buffer;
    int i;

    if (!filename || !filename[0]) return qfalse;
    if (strrchr(filename, '.')) Com_sprintf(path, sizeof(path), "%s", filename);
    else Com_sprintf(path, sizeof(path), "%s.mus", filename);

    if (FS_ReadFile(path, (void **)&data) <= 0 || !data) {
        Com_DPrintf("PS3 audio: couldn't load soundtrack definition %s\n", path);
        return qfalse;
    }

    S_PS3AUDIO_StopStream(0);
    ps3_music_num_songs = 0;
    ps3_music_song = -1;
    ps3_music_last_failed = -1;
    loadPath[0] = '\0';
    buffer = (char *)data;

    while (buffer && *buffer) {
        int numArgs = 1;
        char alias[MAX_RES_NAME];
        char file[MAX_RES_NAME];

        Q_strncpyz(token, COM_GetToken(&buffer, qtrue), sizeof(token));
        if (!token[0]) break;
        memset(args, 0, sizeof(args));
        Q_strncpyz(args[0], token, sizeof(args[0]));

        while (numArgs < 4) {
            Q_strncpyz(token, COM_GetToken(&buffer, qfalse), sizeof(token));
            if (!token[0]) break;
            Q_strncpyz(args[numArgs++], token, sizeof(args[0]));
        }

        if (!Q_stricmp(args[0], "path")) {
            if (numArgs < 2) continue;
            Q_strncpyz(loadPath, args[1], sizeof(loadPath));
            if (loadPath[0]) {
                size_t len = strlen(loadPath);
                if (loadPath[len - 1] != '/' && loadPath[len - 1] != '\\')
                    Q_strcat(loadPath, sizeof(loadPath), "/");
            }
            continue;
        }

        if (args[0][0] == '!') {
            song_t *song = NULL;
            for (i = 0; i < ps3_music_num_songs; i++) {
                if (!Q_stricmp(ps3_music_songs[i].alias, &args[0][1])) {
                    song = &ps3_music_songs[i];
                    break;
                }
            }
            if (!song || numArgs < 2) continue;
            if (!Q_stricmp(args[1], "volume") && numArgs >= 3) song->volume = (float)atof(args[2]);
            else if (!Q_stricmp(args[1], "fadetime") && numArgs >= 3) song->fadetime = (float)atof(args[2]);
            else if (!Q_stricmp(args[1], "loop")) song->flags |= 1;
            else if (!Q_stricmp(args[1], "restart")) song->flags |= 2;
            else if (!Q_stricmp(args[1], "interrupt")) { song->fadetime = 0.0f; song->flags |= 4 | 2; }
            continue;
        }

        if (numArgs >= 2) {
            Q_strncpyz(alias, args[0], sizeof(alias));
            Q_strncpyz(file, loadPath, sizeof(file));
            Q_strcat(file, sizeof(file), args[1]);
        } else {
            size_t len;
            Q_strncpyz(file, loadPath, sizeof(file));
            Q_strcat(file, sizeof(file), args[0]);
            len = strlen(args[0]);
            if (len >= 4) Q_strncpyz(alias, args[0], len - 3);
            else alias[0] = '\0';
        }

        if (ps3_music_num_songs >= PS3_MUSIC_MAX_SONGS) continue;
        Q_strncpyz(ps3_music_songs[ps3_music_num_songs].alias, alias,
                   sizeof(ps3_music_songs[ps3_music_num_songs].alias));
        Q_strncpyz(ps3_music_songs[ps3_music_num_songs].path, file,
                   sizeof(ps3_music_songs[ps3_music_num_songs].path));
        ps3_music_songs[ps3_music_num_songs].volume = 1.0f;
        ps3_music_songs[ps3_music_num_songs].fadetime = 1.0f;
        ps3_music_songs[ps3_music_num_songs].flags = 0;
        ps3_music_songs[ps3_music_num_songs].current_pos = 0;
        ps3_music_songs[ps3_music_num_songs].mood_num = MusicMood_NameToNum(alias);
        ps3_music_num_songs++;
    }

    FS_FreeFile(data);
    ps3_music_loaded = ps3_music_num_songs > 0 ? qtrue : qfalse;
    if (!ps3_music_loaded) {
        Com_DPrintf("PS3 audio: soundtrack %s has no valid entries\n", path);
        return qfalse;
    }
    if (ps3_music_current_mood == mood_none) {
        ps3_music_current_mood = mood_normal;
        ps3_music_fallback_mood = mood_normal;
    }
    return qtrue;
}

qboolean MUSIC_SongValid(const char *mood)
{
    return MUSIC_FindSong(mood) >= 0 ? qtrue : qfalse;
}

qboolean MUSIC_Loaded(void)
{
    return ps3_music_loaded;
}

void Music_Update(void)
{
    PS3_MusicUpdateInternal();
}

void MUSIC_SongEnded(void)
{
    ps3_music_current_mood = ps3_music_fallback_mood;
    ps3_music_song = -1;
}

void MUSIC_NewSoundtrack(const char *name)
{
    if (!name) return;
    if (!Q_stricmp(name, "none") || !name[0]) {
        MUSIC_FreeAllSongs();
        return;
    }
    Q_strncpyz(ps3_current_soundtrack, name, sizeof(ps3_current_soundtrack));
    ps3_music_active = qtrue;
    ps3_music_loaded = qfalse;
    ps3_music_current_mood = mood_none;
    ps3_music_fallback_mood = mood_none;
    (void)MUSIC_LoadSoundtrackFile(name);
}

void MUSIC_UpdateMood(int current, int fallback)
{
    ps3_music_current_mood = current;
    ps3_music_fallback_mood = fallback;
    ps3_music_active = qtrue;
    ps3_music_last_failed = -1;
}

void MUSIC_UpdateVolume(float volume, float fade_time)
{
    ps3_music_volume_start = ps3_music_volume;
    ps3_music_volume_target = volume;
    ps3_music_fade_seconds = fade_time;
    ps3_music_fade_start = cls.realtime;
    if (fade_time <= 0.0f) {
        ps3_music_volume = volume;
        ps3_music_fade_seconds = 0.0f;
    }
    PS3_MusicApplyVolume();
}

void MUSIC_StopAllSongs(void)
{
    S_PS3AUDIO_StopStream(0);
    ps3_music_song = -1;
}

void MUSIC_FreeAllSongs(void)
{
    MUSIC_StopAllSongs();
    ps3_music_current_mood = mood_none;
    ps3_music_fallback_mood = mood_none;
    ps3_current_soundtrack[0] = '\0';
    ps3_music_num_songs = 0;
    ps3_music_loaded = qfalse;
    ps3_music_active = qfalse;
}

qboolean MUSIC_Playing(void)
{
    return S_PS3AUDIO_IsStreamActive(0);
}

int MUSIC_FindSong(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < ps3_music_num_songs; i++)
        if (!Q_stricmp(ps3_music_songs[i].alias, name)) return i;
    return -1;
}

void S_loadsoundtrack(void)
{
    if (Cmd_Argc() != 2) {
        Com_Printf("loadsoundtrack <sound track file>\n");
        return;
    }
    if (MUSIC_LoadSoundtrackFile(Cmd_Argv(1))) {
        Q_strncpyz(ps3_current_soundtrack, Cmd_Argv(1), sizeof(ps3_current_soundtrack));
        ps3_music_active = qtrue;
    }
}

const char *S_CurrentSoundtrack(void)
{
    return ps3_current_soundtrack;
}

void S_PlaySong(void)
{
    if (Cmd_Argc() != 2) {
        Com_Printf("playsong <song alias>\n");
        return;
    }
    (void)MUSIC_PlaySong(Cmd_Argv(1));
    ps3_music_active = qtrue;
}

int MUSIC_CurrentSongChannel(void)
{
    return S_PS3AUDIO_IsStreamActive(0) ? 0 : -1;
}

void MUSIC_StopChannel(int channel_number)
{
    if (channel_number != 0) return;
    if (ps3_music_song >= 0 && ps3_music_song < ps3_music_num_songs) {
        if (ps3_music_songs[ps3_music_song].flags & 2)
            ps3_music_songs[ps3_music_song].current_pos = 0;
        else
            ps3_music_songs[ps3_music_song].current_pos = (int)S_PS3AUDIO_GetStreamPosition(0);
    }
    S_PS3AUDIO_StopStream(0);
    ps3_music_song = -1;
}

qboolean MUSIC_PlaySong(const char *alias)
{
    int song = MUSIC_FindSong(alias);
    if (song < 0) return qfalse;
    ps3_music_active = qtrue;
    ps3_music_current_mood = ps3_music_songs[song].mood_num;
    ps3_music_last_failed = -1;
    return PS3_MusicStartSong(song);
}

void MUSIC_UpdateMusicVolumes(void)
{
    PS3_MusicApplyVolume();
}

void MUSIC_CheckForStoppedSongs(void)
{
    if (ps3_music_song >= 0 && !S_PS3AUDIO_IsStreamActive(0)) {
        if (!(ps3_music_songs[ps3_music_song].flags & 1)) MUSIC_SongEnded();
        ps3_music_song = -1;
    }
}

void S_TriggeredMusic_SetupHandle(const char *name, int loopCount, int offset, qboolean autostart)
{
    const char *realName;
    AliasListNode_t *alias = NULL;
    float volume = 1.0f;
    if (!name || !name[0]) return;
    realName = Alias_FindRandom(name, &alias);
    if (realName && alias) {
        volume = alias->volume + (float)rand() / (float)RAND_MAX * alias->volumeMod;
    } else {
        realName = name;
    }
    if (S_PS3AUDIO_StartStream(1, realName, loopCount, (unsigned int)(offset < 0 ? 0 : offset),
            volume * (s_musicVolume ? s_musicVolume->value : 0.9f), 1.0f,
            qtrue, NULL, 0, CHAN_LOCAL, 1.0f, 1.0f) && !autostart) {
        S_PS3AUDIO_PauseStream(1, qtrue);
    }
}

void S_TriggeredMusic_Start(void)
{
    if (Cmd_Argc() != 2) { Com_Printf("tmstart <sound file>\n"); return; }
    S_TriggeredMusic_SetupHandle(Cmd_Argv(1), 1, 0, qtrue);
}

void S_TriggeredMusic_StartLoop(void)
{
    if (Cmd_Argc() != 2) { Com_Printf("tmstartloop <sound file>\n"); return; }
    S_TriggeredMusic_SetupHandle(Cmd_Argv(1), 0, 0, qtrue);
}

void S_TriggeredMusic_Stop(void)
{
    S_PS3AUDIO_StopStream(1);
}

void S_TriggeredMusic_Volume(void)
{
    if (Cmd_Argc() != 2) { Com_Printf("tmvolume <0-1>\n"); return; }
    S_PS3AUDIO_SetStreamVolume(1, (float)atof(Cmd_Argv(1)));
}

void S_TriggeredMusic_Pause(void)
{
    S_PS3AUDIO_PauseStream(1, qtrue);
}

void S_TriggeredMusic_Unpause(void)
{
    S_PS3AUDIO_PauseStream(1, qfalse);
}

void S_TriggeredMusic_PlayIntroMusic(void)
{
    S_TriggeredMusic_SetupHandle("sound/music/mus_MainTheme.mp3", 0, 0, qtrue);
}

void S_StopMovieAudio(void)
{
    S_PS3AUDIO_StopStream(2);
}

void S_SetupMovieAudio(const char *movieName)
{
    char filename[MAX_QPATH];
    if (!movieName || !movieName[0]) return;
    COM_StripExtension(movieName, filename, sizeof(filename));
    Q_strcat(filename, sizeof(filename), ".mp3");
    if (!S_PS3AUDIO_StartStream(2, filename, 1, 0, 1.5f, 1.0f,
            qtrue, NULL, 0, CHAN_LOCAL, 1.0f, 1.0f)) {
        COM_StripExtension(movieName, filename, sizeof(filename));
        Q_strcat(filename, sizeof(filename), ".wav");
        if (!S_PS3AUDIO_StartStream(2, filename, 1, 0, 1.5f, 1.0f,
                qtrue, NULL, 0, CHAN_LOCAL, 1.0f, 1.0f)) {
            Com_DPrintf("PS3 audio: no MP3/WAV movie companion for %s\n", movieName);
        }
    }
}

int S_CurrentMoviePosition(void)
{
    return (int)S_PS3AUDIO_GetStreamPosition(2);
}

void S_PrintInfo(void)
{
    Com_Printf("PS3 native libaudio: %s, streams=%d, soundtrack=%s\n",
        S_PS3AUDIO_IsStreamActive(0) ? "playing" : "ready",
        ps3_music_num_songs, ps3_current_soundtrack);
}

void S_DumpInfo(void)
{
    S_PrintInfo();
}

qboolean S_NeedFullRestart(void) { return qfalse; }
void S_SaveData(soundsystemsavegame_t *save) { (void)save; }
void S_LoadData(soundsystemsavegame_t *save) { (void)save; }
void S_ReLoad(soundsystemsavegame_t *save) { (void)save; }

}
