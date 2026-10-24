#include "net/bot.h"

namespace net {

wire::PlayerState bot_state(PlayerId player, uint64_t now_ms, const BotPlan& plan) {
    wire::PlayerState s;
    // Start points run along x, one spacing apart, so the bots are visibly separate Links.
    s.x = static_cast<int32_t>(player - kFirstPlayer) * plan.spacing;
    s.y = 0;
    s.z = 0;
    s.yaw = 0;

    const uint64_t phase = now_ms % plan.period_ms;
    const uint64_t walk = plan.walk_ms;
    int64_t offset = 0; // distance walked from the start point, in game units
    if (phase < walk) {
        offset = static_cast<int64_t>(plan.speed) * static_cast<int64_t>(phase) / 1000;
        s.anim = kAnimWalkForward;
    } else if (phase < 2 * walk) {
        const uint64_t back = 2 * walk - phase; // time left to reach the start point again
        offset = static_cast<int64_t>(plan.speed) * static_cast<int64_t>(back) / 1000;
        s.anim = kAnimWalkBack;
    } else {
        s.anim = kAnimIdle;
    }
    s.z = static_cast<int32_t>(offset);
    return s;
}

} // namespace net
