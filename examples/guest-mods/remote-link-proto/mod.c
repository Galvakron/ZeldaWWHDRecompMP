/* Prototype: three static remote Links, spaced apart from the real Link.
 *
 * Each proxy stands at (real_Link_x + (N+1)*SPACING, real_Link_y, real_Link_z) where N is proxy index
 * 0..2. They do not move — this is a spawn-and-place test. Once they appear, we know the profile id
 * is correct and we can add movement, tints and network input later.
 *
 * The profile id 0xA9 (169) comes from the GC Wind Waker decomp (zeldaret/tww), where Link is the 170th
 * entry in g_fpcPfLst_ProfileList (index 169). The WWHD engine keeps the same profile table. A discovery
 * hook confirms this at runtime by logging the procName when daPy_Create is called; if 0xA9 is wrong, the
 * log will show the real value.
 *
 * Field offsets come from wwhd/actor.h (fopAc_ac_c.current at 0x314, angle at 0x320, room at 0x326). */

#include "wwhd_guest.h"
#include "wwhd/functions.h"
#include "wwhd/bindings.h"
#include "wwhd/vectors.h"

/* Link's profile id — index 169 in g_fpcPfLst_ProfileList (GC WW decomp). */
#define REMOTE_LINK_PROC 0xA9u

#define PROXY_COUNT 3
#define SPACING 400  /* game units along x between each proxy */

#define OFF_CURRENT_POS   0x314
#define OFF_CURRENT_ANGLE 0x320
#define OFF_CURRENT_ROOM  0x326

static u32  proxy_actor[PROXY_COUNT];
static int  spawned;
static int  spawn_failed;

/* ---- position helpers ---- */

static void write_position(u8* actor, float x, float y, float z, s16 yaw) {
    cXyz*  pos   = (cXyz*)(actor + OFF_CURRENT_POS);
    csXyz* angle = (csXyz*)(actor + OFF_CURRENT_ANGLE);
    pos->x = x;
    pos->y = y;
    pos->z = z;
    angle->x = 0;
    angle->y = yaw;
    angle->z = 0;
}

/* ---- spawning ---- */

static void spawn_proxies(u8* link) {
    cXyz* p = (cXyz*)(link + OFF_CURRENT_POS);
    float bx = p->x;
    float by = p->y;
    float bz = p->z;
    s8 room  = *(s8*)(link + OFF_CURRENT_ROOM);
    spawned = 1;

    wwhd_log("remote-link-proto: spawning 3 static proxies");

    for (unsigned i = 0; i < PROXY_COUNT; ++i) {
        cXyz at;
        csXyz angle = {0, 0, 0};
        at.x = bx + (float)((int)(i + 1) * SPACING);
        at.y = by;
        at.z = bz;
        u32 id = wwhd_fopAcM_create(REMOTE_LINK_PROC, 0,
                                     (u32)(unsigned long)&at, (s32)room,
                                     (u32)(unsigned long)&angle, 0, 0, 0);
        if (id == 0) {
            wwhd_log_int("remote-link-proto: create returned 0 for proxy", (int)i);
            spawn_failed = 1;
            continue;
        }
        wwhd_log_int("remote-link-proto: created proxy with unique id", (int)id);

        /* Look up the actor pointer from the unique id. */
        u32 actor = 0;
        wwhd_fopAcM_SearchByID(id, (u32)(unsigned long)&actor);
        if (actor == 0) {
            wwhd_log_int("remote-link-proto: SearchByID failed for proxy", (int)i);
            spawn_failed = 1;
            continue;
        }
        proxy_actor[i] = actor;
        wwhd_log_int("remote-link-proto: proxy actor ready", (int)i);
    }

    if (!spawn_failed)
        wwhd_log("remote-link-proto: all 3 proxies spawned OK");
}

/* ---- discovery hook: confirm the profile id at runtime ---- */

WWHD_HOOK(WWHD_ADDR_fopAcM_create, log_link_profile,
          (u32 procName, u32 parameter, u32 pPos, s32 roomNo,
           u32 pAngle, u32 pScale, u32 i_argument, u32 createFunc)) {
    if (createFunc == WWHD_ADDR_daPy_Create)
        wwhd_log_hex("remote-link-proto: Link profile id", procName);
}

/* ---- per-frame: spawn once, then keep proxies in place ---- */

WWHD_HOOK(WWHD_ADDR_daPy_Execute, remote_link_step, (void* link)) {
    if (!spawned) {
        spawn_proxies((u8*)link);
        return;
    }

    /* Re-read the real Link position every frame and place the proxies relative to it.
     * This keeps them visible even as the player walks around. */
    u8* lk = (u8*)link;
    cXyz* p = (cXyz*)(lk + OFF_CURRENT_POS);
    float bx = p->x;
    float by = p->y;
    float bz = p->z;

    for (unsigned i = 0; i < PROXY_COUNT; ++i) {
        u8* actor = (u8*)(unsigned long)proxy_actor[i];
        if (actor == 0) continue;
        write_position(actor, bx + (float)((int)(i + 1) * SPACING), by, bz, 0);
    }
}
