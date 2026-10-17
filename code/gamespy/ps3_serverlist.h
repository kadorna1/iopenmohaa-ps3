/* PS3 compatibility subset for the legacy GameSpy server-list API. */
#ifndef PS3_SERVERLIST_H
#define PS3_SERVERLIST_H

#include "goaceng.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The PS3 implementation intentionally keeps this API surface but performs
 * no GameSpy master/server queries. The concrete list state is private to
 * ps3_serverlist.c so the public GameSpy ABI remains unchanged. */

#ifdef __cplusplus
}
#endif

#endif /* PS3_SERVERLIST_H */
