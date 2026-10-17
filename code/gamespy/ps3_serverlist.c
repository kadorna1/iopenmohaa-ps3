/* PS3 GameSpy server-list compatibility implementation. */
#include "ps3_serverlist.h"

#include <stdlib.h>

struct GServerListImplementation {
    ListCallBackFn callback;
    void *instance;
    GServerListState state;
};

/* goaceng.h declares this global and the original gserverlist.c provides it
 * on desktop. The PS3 build excludes that legacy implementation. */
char ServerListHostname[64];

GServerList ServerListNew(const char *gamename, const char *enginename,
                          const char *seckey, int maxconcupdates,
                          void *CallBackFn, int CallBackFnType, void *instance)
{
    struct GServerListImplementation *list;

    (void)gamename;
    (void)enginename;
    (void)seckey;
    (void)maxconcupdates;
    (void)CallBackFnType;

    list = (struct GServerListImplementation *)calloc(1, sizeof(*list));
    if (!list)
        return NULL;

    list->callback = (ListCallBackFn)CallBackFn;
    list->instance = instance;
    list->state = sl_idle;
    return list;
}

void ServerListFree(GServerList serverlist)
{
    free(serverlist);
}

GError ServerListUpdate(GServerList serverlist, gbool async)
{
    (void)async;
    if (serverlist && serverlist->callback)
        serverlist->callback(serverlist, LIST_STATECHANGED, serverlist->instance, NULL, NULL);
    return GE_NOERROR;
}

GError ServerListUpdate2(GServerList serverlist, gbool async,
                         char *filter, GQueryType querytype)
{
    (void)filter;
    (void)querytype;
    return ServerListUpdate(serverlist, async);
}

GError ServerListLANUpdate(GServerList serverlist, gbool async,
                           int startsearchport, int endsearchport, int searchdelta)
{
    (void)async;
    (void)startsearchport;
    (void)endsearchport;
    (void)searchdelta;
    if (serverlist && serverlist->callback)
        serverlist->callback(serverlist, LIST_STATECHANGED, serverlist->instance, NULL, NULL);
    return GE_NOERROR;
}

GError ServerListAuxUpdate(GServerList serverlist, const char *ip, int port,
                           gbool async, GQueryType querytype)
{
    (void)ip;
    (void)port;
    (void)async;
    (void)querytype;
    if (serverlist && serverlist->callback)
        serverlist->callback(serverlist, LIST_STATECHANGED, serverlist->instance, NULL, NULL);
    return GE_NOERROR;
}

GError ServerListThink(GServerList serverlist)
{
    (void)serverlist;
    return GE_NOERROR;
}

GError ServerListHalt(GServerList serverlist)
{
    (void)serverlist;
    return GE_NOERROR;
}

GError ServerListClear(GServerList serverlist)
{
    (void)serverlist;
    return GE_NOERROR;
}

int ServerListCount(GServerList serverlist)
{
    (void)serverlist;
    return 0;
}

GServer ServerListGetServer(GServerList serverlist, int index)
{
    (void)serverlist;
    (void)index;
    return NULL;
}

int ServerGetPing(GServer server)
{
    (void)server;
    return 0;
}

char *ServerGetAddress(GServer server)
{
    (void)server;
    return (char *)"0.0.0.0";
}

int ServerGetQueryPort(GServer server)
{
    (void)server;
    return 0;
}

char *ServerGetStringValue(GServer server, char *key, char *sdefault)
{
    (void)server;
    (void)key;
    return sdefault;
}

int ServerGetIntValue(GServer server, char *key, int idefault)
{
    (void)server;
    (void)key;
    return idefault;
}

GServerListState ServerListState(GServerList serverlist)
{
    if (!serverlist)
        return sl_idle;
    return serverlist->state;
}

void ServerListRemoveServer(GServerList serverlist, char *ip, int port)
{
    (void)serverlist;
    (void)ip;
    (void)port;
}
