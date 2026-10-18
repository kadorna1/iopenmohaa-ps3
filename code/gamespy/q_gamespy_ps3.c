/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

PS3 GameSpy compatibility layer. GameSpy's original server-side SDK is not
part of the PS3 build; keep the common master-server API used by the client
and make the legacy server restart hook a harmless no-op.
===========================================================================
*/

#include "q_gamespy.h"
#include "../qcommon/qcommon.h"
#include "../qcommon/q_version.h"
#include "cl_gamespy.h"

#define MAX_MASTERS           8
#define MASTER_DEFAULT_MSPORT 28900
#define MASTER_DEFAULT_HBPORT 27900

static cvar_t *com_master_host[MAX_MASTERS];
static cvar_t *com_master_msport[MAX_MASTERS];
static cvar_t *com_master_hbport[MAX_MASTERS];

static master_entry_t entries[MAX_MASTERS];
static unsigned int num_entries;

static const char *PS3_SECRET_GS_KEYS[] = { "M5Fdwc", "h2P1c9", "y32FDc" };
static const unsigned int PS3_GS_GAME_IDS[] = { 0, 641, 802 };
static const char *PS3_GS_GAME_NAME[] = { "mohaa", "mohaas", "mohaab" };
static const char *PS3_GS_GAME_NAME_DEMO[] = { "mohaa", "mohaabd", "mohaabd" };

const char *GS_GetGameKey(unsigned int index) { if (index >= 3) index = 0; return PS3_SECRET_GS_KEYS[index]; }
unsigned int GS_GetGameID(unsigned int index) { if (index >= 3) index = 0; return PS3_GS_GAME_IDS[index]; }
const char *GS_GetGameName(unsigned int index) {
    if (index >= 3) index = 0;
    return (com_target_demo && com_target_demo->integer) ? PS3_GS_GAME_NAME_DEMO[index] : PS3_GS_GAME_NAME[index];
}
const char *GS_GetCurrentGameKey(void) { return GS_GetGameKey(com_target_game ? (unsigned)com_target_game->integer : 0); }
unsigned int GS_GetCurrentGameID(void) { return GS_GetGameID(com_target_game ? (unsigned)com_target_game->integer : 0); }
const char *GS_GetCurrentGameName(void) { return GS_GetGameName(com_target_game ? (unsigned)com_target_game->integer : 0); }
const char *GS_GetGameVersion(unsigned int index) {
    static const char *versions[] = { TARGET_GAME_VERSION_MOH "+" PRODUCT_VERSION, TARGET_GAME_VERSION_MOHTA "+" PRODUCT_VERSION, TARGET_GAME_VERSION_MOHTT "+" PRODUCT_VERSION };
    static const char *demo_versions[] = { TARGET_GAME_VERSION_MOH "+" PRODUCT_VERSION, "d" TARGET_GAME_VERSION_MOHTA "+" PRODUCT_VERSION, "d" TARGET_GAME_VERSION_MOHTT_DEMO "+" PRODUCT_VERSION };
    if (index >= 3) index = 0;
    return (com_target_demo && com_target_demo->integer) ? demo_versions[index] : versions[index];
}
const char *GS_GetCurrentGameVersion(void) { return GS_GetGameVersion(com_target_game ? (unsigned)com_target_game->integer : 0); }

void SV_CreateGamespyChallenge(char *challenge) { if (challenge) *challenge = '\0'; }
void SV_GamespyAuthorize(netadr_t from, const char *response) { (void)from; (void)response; }
void SV_GamespyHeartbeat(void) {}
void SV_ProcessGamespyQueries(void) {}
extern cvar_t *sv_gamespy;
qboolean SV_InitGamespy(void)
{
    /* The upstream SV_InitGamespy() creates sv_gamespy; this stub replaces it,
     * so create the cvar here. Without it sv_gamespy stays NULL and
     * SV_MasterHeartbeat() dereferences it on the first server frame after a
     * map load (crash). Master heartbeats are no-ops on PS3 either way. */
    sv_gamespy = Cvar_Get("sv_gamespy", "0", CVAR_LATCH);
    return qfalse;
}
void SV_ShutdownGamespy(void) {}
void SV_RestartGamespy(void) {}
void SV_RestartGamespy_f(void) {}
void SV_TryRestartGamespy(void) {}
void SV_GamespyClientDisconnect(int gamespyId) { (void)gamespyId; }

static qboolean ShouldRefreshMasters(void);
static void Com_RestartGameSpy_f(void);

static qboolean ShouldRefreshMasters(void)
{
    int i;

    for (i = 0; i < MAX_MASTERS; ++i) {
        if ((com_master_host[i] && com_master_host[i]->latchedString) ||
            (com_master_msport[i] && com_master_msport[i]->latchedString) ||
            (com_master_hbport[i] && com_master_hbport[i]->latchedString)) {
            return qtrue;
        }
    }

    return qfalse;
}

static void CreateMasterVar(int index, const char *host, int msport, int hbport)
{
    com_master_host[index] = Cvar_Get(va("com_master%d_host", index), host,
                                      CVAR_LATCH | CVAR_TEMP);
    com_master_msport[index] = Cvar_Get(va("com_master%d_msport", index),
                                        va("%d", msport), CVAR_LATCH | CVAR_TEMP);
    com_master_hbport[index] = Cvar_Get(va("com_master%d_hbport", index),
                                        va("%d", hbport), CVAR_LATCH | CVAR_TEMP);

    com_master_host[index]->flags &= ~CVAR_ARCHIVE;
    com_master_msport[index]->flags &= ~CVAR_ARCHIVE;
    com_master_hbport[index]->flags &= ~CVAR_ARCHIVE;
}

qboolean Com_RefreshGameSpyMasters(void)
{
    static const char *const masters[] = {
        "master.333networks.com",
        "master.frag-net.com",
        "master.errorist.eu",
        "master-au.unrealarchive.org",
        "master.noccer.de"
    };
    qboolean should_restart = ShouldRefreshMasters();
    int i;

    num_entries = 0;

    for (i = 0; i < MAX_MASTERS; ++i) {
        const char *host = (i < (int)(sizeof(masters) / sizeof(masters[0])))
            ? masters[i] : "";
        CreateMasterVar(i, host, MASTER_DEFAULT_MSPORT, MASTER_DEFAULT_HBPORT);
        if (host[0]) {
            entries[num_entries].host = com_master_host[i]->string;
            entries[num_entries].queryport = com_master_msport[i]->integer;
            entries[num_entries].hbport = com_master_hbport[i]->integer;
            ++num_entries;
        }
    }

    return should_restart;
}

static void Com_RestartGameSpy_f(void)
{
    Com_RefreshGameSpyMasters();
    CL_RestartGamespy_f();
}

void Com_InitGameSpy(void)
{
    Com_RefreshGameSpyMasters();
    Cmd_AddCommand("net_gamespy_restart", Com_RestartGameSpy_f);
}

unsigned int Com_GetNumMasterEntries(void)
{
    return num_entries;
}

void Com_GetMasterEntry(int index, master_entry_t *entry)
{
    if (!entry) {
        return;
    }

    if (index < 0 || index >= (int)num_entries) {
        entry->host = NULL;
        entry->queryport = 0;
        entry->hbport = 0;
        return;
    }

    *entry = entries[index];
}

const char *Com_GetMasterHost(void)
{
    return num_entries ? entries[0].host : "";
}

int Com_GetMasterQueryPort(void)
{
    return num_entries ? entries[0].queryport : MASTER_DEFAULT_MSPORT;
}

int Com_GetMasterHeartbeatPort(void)
{
    return num_entries ? entries[0].hbport : MASTER_DEFAULT_HBPORT;
}
