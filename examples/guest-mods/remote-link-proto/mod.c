/* Prototype v4: deferred SearchByID — wait frames after fopAcM_create before looking up the actor.
 *
 * fopAcM_create returns a unique id but the actor is not constructed until a later frame. We now
 * spawn on frame 30, then poll SearchByID every frame until all 3 actors are found (or 120 frames
 * have passed). Once found, we place each proxy at (Link_x + N*400, Link_y, Link_z) every frame.
 *
 * Profile id 0xA9 (Link, index 169 from GC decomp). The discovery hook watches createFunc but in
 * WWHD all observed createFuncs are 0, so profiling is purely by procName.
 *
 * Field offsets from wwhd/actor.h (fopAc_ac_c.current at 0x314, angle at 0x320, room at 0x326). */

#include "wwhd_guest.h"
#include "wwhd/functions.h"
#include "wwhd/bindings.h"
#include "wwhd/vectors.h"

#define LINK_PROC   0xA9u
#define PROXY_COUNT 3
#define SPACING     400

#define OFF_CURRENT_POS   0x314
#define OFF_CURRENT_ANGLE 0x320
#define OFF_CURRENT_ROOM  0x326

/* Lifecycle states. */
#define STATE_WAIT       0   /* waiting for frame 30 */
#define STATE_CREATED    1   /* fopAcM_create done, polling SearchByID */
#define STATE_READY      2   /* all actors resolved */
#define STATE_FAILED     3   /* gave up */

static int   state;
static int   frame_count;
static u32   proxy_uid[PROXY_COUNT];    /* unique ids from fopAcM_create */
static u32   proxy_actor[PROXY_COUNT];  /* actor pointers from SearchByID */
static int   resolved_count;
static int   poll_frames;

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

/* ---- create requests (returns immediately; actors built later) ---- */

static void request_spawn(u8* link) {
    cXyz* p = (cXyz*)(link + OFF_CURRENT_POS);
    float bx = p->x;
    float by = p->y;
    float bz = p->z;
    s8 room  = *(s8*)(link + OFF_CURRENT_ROOM);

    wwhd_log("remote-link-proto: requesting 3 proxies");
    wwhd_log_int("remote-link-proto: room", (int)room);

    for (unsigned i = 0; i < PROXY_COUNT; ++i) {
        cXyz at;
        csXyz angle = {0, 0, 0};
        at.x = bx + (float)((int)(i + 1) * SPACING);
        at.y = by;
        at.z = bz;
        u32 uid = wwhd_fopAcM_create(LINK_PROC, 0,
                                      (u32)(unsigned long)&at, (s32)room,
                                      (u32)(unsigned long)&angle, 0, 0, 0);
        proxy_uid[i] = uid;
        wwhd_log_int("remote-link-proto: create uid", (int)uid);
    }
    state = STATE_CREATED;
}

/* ---- poll until actors are ready ---- */

static void poll_actors(void) {
    poll_frames++;
    for (unsigned i = 0; i < PROXY_COUNT; ++i) {
        if (proxy_actor[i] != 0) continue;    /* already resolved */
        if (proxy_uid[i] == 0) continue;       /* create failed */

        u32 actor = 0;
        wwhd_fopAcM_SearchByID(proxy_uid[i], (u32)(unsigned long)&actor);
        if (actor != 0) {
            proxy_actor[i] = actor;
            resolved_count++;
            wwhd_log_int("remote-link-proto: resolved proxy", (int)i);
            wwhd_log_hex("remote-link-proto:   actor ptr", actor);
        }
    }

    if (resolved_count >= PROXY_COUNT) {
        wwhd_log("remote-link-proto: ALL proxies resolved!");
        state = STATE_READY;
    } else if (poll_frames > 120) {
        wwhd_log_int("remote-link-proto: gave up polling, resolved", resolved_count);
        state = (resolved_count > 0) ? STATE_READY : STATE_FAILED;
    }
}

/* ---- discovery: log all actor creations (first 30) ---- */

static int create_log_count;

WWHD_HOOK(WWHD_ADDR_fopAcM_create, log_actor_create,
          (u32 procName, u32 parameter, u32 pPos, s32 roomNo,
           u32 pAngle, u32 pScale, u32 i_argument, u32 createFunc)) {
    if (create_log_count < 30) {
        wwhd_log_hex("remote-link-proto: fopAcM_create procName", procName);
        create_log_count++;
    }
}

/* ---- per-frame ---- */

WWHD_HOOK(WWHD_ADDR_daPy_Execute, remote_link_step, (void* link)) {
    frame_count++;

    if (frame_count == 1) {
        wwhd_log("remote-link-proto: daPy_Execute hook is RUNNING (v4)");
    }

    switch (state) {
    case STATE_WAIT:
        if (frame_count >= 30) {
            request_spawn((u8*)link);
        }
        break;

    case STATE_CREATED:
        poll_actors();
        break;

    case STATE_READY: {
        /* Place proxies relative to real Link every frame. */
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
        break;
    }

    case STATE_FAILED:
        /* Nothing to do. */
        break;
    }
}
