/* Prototype v3: diagnostic version that logs everything.
 *
 * Phase 1 — Discovery: logs every fopAcM_create call so we can see which actors exist and what their
 * profile ids are. Also specifically checks for daPy_Create to confirm Link's profile id.
 *
 * Phase 2 — Spawn: once we have confirmed the id, spawn 3 static proxies spaced 400 units apart.
 * If Link spawn fails (singleton), we try the SHIP actor (index 167 in GC decomp = 0xA7) as a
 * visible fallback.
 *
 * Field offsets from wwhd/actor.h (fopAc_ac_c.current at 0x314, angle at 0x320, room at 0x326). */

#include "wwhd_guest.h"
#include "wwhd/functions.h"
#include "wwhd/bindings.h"
#include "wwhd/vectors.h"

/* Profile ids from GC Wind Waker decomp (zeldaret/tww g_fpcPfLst_ProfileList).
 * Link = 169 (0xA9), Ship = 167 (0xA7). WWHD may differ — the discovery hook tells us. */
#define LINK_PROC   0xA9u
#define SHIP_PROC   0xA7u

#define PROXY_COUNT 3
#define SPACING     400

#define OFF_CURRENT_POS   0x314
#define OFF_CURRENT_ANGLE 0x320
#define OFF_CURRENT_ROOM  0x326

static u32  proxy_actor[PROXY_COUNT];
static int  spawned;
static int  frame_count;
static u32  discovered_link_proc;   /* filled by the discovery hook */
static int  create_log_count;       /* limit logging to avoid flooding */

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

/* ---- try to spawn a single actor with a given profile id ---- */

static u32 try_spawn(u32 proc, float x, float y, float z, s8 room) {
    cXyz at;
    csXyz angle = {0, 0, 0};
    at.x = x;
    at.y = y;
    at.z = z;

    wwhd_log_hex("remote-link-proto: trying spawn with proc", proc);

    u32 id = wwhd_fopAcM_create(proc, 0,
                                 (u32)(unsigned long)&at, (s32)room,
                                 (u32)(unsigned long)&angle, 0, 0, 0);
    if (id == 0) {
        wwhd_log("remote-link-proto:   -> fopAcM_create returned 0 (FAILED)");
        return 0;
    }
    wwhd_log_int("remote-link-proto:   -> unique id", (int)id);

    u32 actor = 0;
    wwhd_fopAcM_SearchByID(id, (u32)(unsigned long)&actor);
    if (actor == 0) {
        wwhd_log("remote-link-proto:   -> SearchByID returned NULL (actor not ready yet?)");
    } else {
        wwhd_log_hex("remote-link-proto:   -> actor pointer", actor);
    }
    return actor;
}

/* ---- spawning ---- */

static void spawn_proxies(u8* link) {
    cXyz* p = (cXyz*)(link + OFF_CURRENT_POS);
    float bx = p->x;
    float by = p->y;
    float bz = p->z;
    s8 room  = *(s8*)(link + OFF_CURRENT_ROOM);

    wwhd_log("remote-link-proto: ========== SPAWN ATTEMPT ==========");
    wwhd_log_int("remote-link-proto: room", (int)room);

    /* Use the discovered profile id if we have one, otherwise try the hardcoded ones. */
    u32 proc = discovered_link_proc ? discovered_link_proc : LINK_PROC;
    wwhd_log_hex("remote-link-proto: using profile id", proc);

    int ok = 0;
    for (unsigned i = 0; i < PROXY_COUNT; ++i) {
        float px = bx + (float)((int)(i + 1) * SPACING);
        u32 actor = try_spawn(proc, px, by, bz, room);
        if (actor != 0) {
            proxy_actor[i] = actor;
            ok++;
        }
    }

    if (ok == 0 && proc == LINK_PROC) {
        wwhd_log("remote-link-proto: Link spawn failed, trying SHIP (0xA7) as fallback");
        for (unsigned i = 0; i < PROXY_COUNT; ++i) {
            float px = bx + (float)((int)(i + 1) * SPACING);
            u32 actor = try_spawn(SHIP_PROC, px, by, bz, room);
            if (actor != 0) {
                proxy_actor[i] = actor;
                ok++;
            }
        }
    }

    wwhd_log_int("remote-link-proto: proxies spawned", ok);
    spawned = 1;
}

/* ---- discovery hook: log ALL actor creations (first 50), highlight Link ---- */

WWHD_HOOK(WWHD_ADDR_fopAcM_create, log_actor_create,
          (u32 procName, u32 parameter, u32 pPos, s32 roomNo,
           u32 pAngle, u32 pScale, u32 i_argument, u32 createFunc)) {

    if (create_log_count < 50) {
        wwhd_log_hex("remote-link-proto: fopAcM_create procName", procName);
        wwhd_log_hex("remote-link-proto:   createFunc", createFunc);
        create_log_count++;
    }

    if (createFunc == WWHD_ADDR_daPy_Create) {
        wwhd_log_hex("remote-link-proto: *** LINK PROFILE ID ***", procName);
        discovered_link_proc = procName;
    }
}

/* ---- per-frame ---- */

WWHD_HOOK(WWHD_ADDR_daPy_Execute, remote_link_step, (void* link)) {
    frame_count++;

    /* Log once to confirm the hook runs. */
    if (frame_count == 1) {
        wwhd_log("remote-link-proto: daPy_Execute hook is RUNNING");
    }

    /* Wait a few frames for the scene to stabilize before spawning. */
    if (!spawned && frame_count >= 30) {
        spawn_proxies((u8*)link);
        return;
    }

    if (!spawned) return;

    /* Keep proxies next to the real Link. */
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
