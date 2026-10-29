/* guids.c - DirectPlay's interface and address-type GUIDs (on Windows they come from dxguid.lib):
 * every DEFINE_GUID in dplay.h and dplobby.h, defined once here. Weak: a game that defines some of
 * them itself (rogan2's dxguid.cpp, "selectany") links without duplicates. */
#include "winweb.h"
#include "initguid.h"
#undef DEFINE_GUID
#define DEFINE_GUID(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    __attribute__((weak)) const GUID n = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#include "dplay.h"
#include "dplobby.h"
/* (dplaysp.h's GUIDs are defined by dplayx itself: dplaysp.c includes initguid.h) */
