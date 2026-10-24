/* Prototype: three remote Links that walk on their own, to test the Link proxies in the game.
 *
 * Each proxy follows the movement bot (runtime/src/net/bot.cpp): every 15 seconds it walks forward for
 * 3 seconds, walks back to its start, then stands still. Proxy N starts (N)*400 units from the real
 * Link, along x. The bot runs inside the mod for now; the network state replaces it later.
 *
 * NOT VERIFIED IN THE GAME YET. The Link profile id (REMOTE_LINK_PROC) is not in the SDK headers, so
 * the proxies are not created until it is set. The field offsets come from wwhd/actor.h
 * (fopAc_ac_c.current at 0x314, its angle at 0x320, its room at 0x326). */

#include "wwhd_guest.h"
#include "wwhd/functions.h"
#include "wwhd/bindings.h"
#include "wwhd/vectors.h"

/* TODO: the profile id of the player actor. Set it from the game's profile table once it is found. */
#define REMOTE_LINK_PROC 0u

#define PROXY_COUNT 3
#define PERIOD_MS 15000u
#define WALK_MS 3000u
#define SPEED 300u   /* game units per second */
#define SPACING 400  /* along x, from the real Link's start */

#define OFF_CURRENT_POS 0x314
#define OFF_CURRENT_ANGLE 0x320
#define OFF_CURRENT_ROOM 0x326

static u32 proxy_actor[PROXY_COUNT];
static int spawned;
static double elapsed_ms;
static float base_x, base_y, base_z;
static s8 base_room;

/* The bot's offset from its start point at time t (ms), along z, and its anim (0 idle, 1 forward, 2 back). */
static void bot_offset(unsigned index, unsigned long long t_ms, s32* dx, s32* dz, u16* anim) {
    unsigned long long phase = t_ms % PERIOD_MS;
    long long offset = 0;
    *dx = (s32)((index + 1) * SPACING);
    if (phase < WALK_MS) {
        offset = (long long)SPEED * (long long)phase / 1000;
        *anim = 1;
    } else if (phase < 2 * WALK_MS) {
        unsigned long long back = 2 * WALK_MS - phase;
        offset = (long long)SPEED * (long long)back / 1000;
        *anim = 2;
    } else {
        *anim = 0;
    }
    *dz = (s32)offset;
}

static void write_position(u8* actor, float x, float y, float z, s16 yaw) {
    cXyz* pos = (cXyz*)(actor + OFF_CURRENT_POS);
    csXyz* angle = (csXyz*)(actor + OFF_CURRENT_ANGLE);
    pos->x = x;
    pos->y = y;
    pos->z = z;
    angle->x = 0;
    angle->y = yaw;
    angle->z = 0;
}

static void spawn_proxies(u8* link) {
    cXyz* p = (cXyz*)(link + OFF_CURRENT_POS);
    base_x = p->x;
    base_y = p->y;
    base_z = p->z;
    base_room = *(s8*)(link + OFF_CURRENT_ROOM);
    spawned = 1;

    if (REMOTE_LINK_PROC == 0) {
        wwhd_log("remote-link-proto: profile id not set, no proxies created");
        return;
    }
    for (unsigned i = 0; i < PROXY_COUNT; ++i) {
        cXyz at;
        csXyz angle = {0, 0, 0};
        at.x = base_x;
        at.y = base_y;
        at.z = base_z;
        u32 id = wwhd_fopAcM_create(REMOTE_LINK_PROC, 0, (u32)(unsigned long)&at, base_room,
                                    (u32)(unsigned long)&angle, 0, 0, 0);
        if (id == 0) {
            wwhd_log_int("remote-link-proto: create failed for proxy", (int)i);
            continue;
        }
        u32 actor = 0;
        wwhd_fopAcM_SearchByID(id, (u32)(unsigned long)&actor);
        proxy_actor[i] = actor;
    }
}

/* Discovery: the game creates Link through fopAcM_create with daPy_Create as the create function. Logging
 * the procName of that call gives the profile id to put in REMOTE_LINK_PROC. */
WWHD_HOOK(WWHD_ADDR_fopAcM_create, log_link_profile,
          (u32 procName, u32 parameter, u32 pPos, s32 roomNo, u32 pAngle, u32 pScale, u32 i_argument, u32 createFunc)) {
    if (createFunc == WWHD_ADDR_daPy_Create) wwhd_log_hex("remote-link-proto: Link profile id", procName);
}

WWHD_HOOK(WWHD_ADDR_daPy_Execute, remote_link_step, (void* link)) {
    if (!spawned) spawn_proxies((u8*)link);

    elapsed_ms += wwhd_logic_dt() * 1000.0;
    unsigned long long t = (unsigned long long)elapsed_ms;

    for (unsigned i = 0; i < PROXY_COUNT; ++i) {
        u8* actor = (u8*)(unsigned long)proxy_actor[i];
        if (actor == 0) continue;
        s32 dx, dz;
        u16 anim;
        bot_offset(i, t, &dx, &dz, &anim);
        write_position(actor, base_x + (float)dx, base_y, base_z + (float)dz, 0);
    }
}
