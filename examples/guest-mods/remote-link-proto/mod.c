/* Prototype v5: VISUAL TEST — lift Link 200 units to confirm the hook is running.
 * If Link floats in the air, the mod is loading. Then we can re-add proxy spawning. */

#include "wwhd_guest.h"
#include "wwhd/functions.h"
#include "wwhd/bindings.h"
#include "wwhd/vectors.h"

#define OFF_CURRENT_POS   0x314

static int frame_count;

WWHD_HOOK(WWHD_ADDR_daPy_Execute, remote_link_step, (void* link)) {
    frame_count++;

    if (frame_count == 1) {
        wwhd_log("remote-link-proto v5: HOOK IS RUNNING — lifting Link!");
    }

    /* After 60 frames, add 200 to Link's Y every frame so he floats. */
    if (frame_count > 60) {
        u8* lk = (u8*)link;
        cXyz* pos = (cXyz*)(lk + OFF_CURRENT_POS);
        pos->y += 200.0f;
    }
}
