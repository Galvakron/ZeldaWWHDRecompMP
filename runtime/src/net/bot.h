#pragma once
// A test bot for the remote Link proxies: it only moves. Every period it walks forward for a few seconds,
// walks back to where it started, then stands still until the next period. Each bot player has its own
// start point and heading, so four bots never overlap. The state is a pure function of the time, so the
// same clock gives the same positions on every machine.
//
// Nothing here touches the game. The game side reads the PlayerState and drives a Link proxy with it.

#include <cstdint>

#include "net/wire.h"

namespace net {

struct BotPlan {
    uint64_t period_ms = 15000; // one walk every 15 seconds
    uint64_t walk_ms = 3000;    // walking forward, then the same time walking back
    int32_t speed = 300;        // game units per second
    int32_t spacing = 400;      // distance between the start points of consecutive players
};

// Anim codes sent in PlayerState::anim. The game maps them to its own Link animations.
constexpr uint16_t kAnimIdle = 0;
constexpr uint16_t kAnimWalkForward = 1;
constexpr uint16_t kAnimWalkBack = 2;

// The bot state of `player` at `now_ms` (the time since the bot started).
wire::PlayerState bot_state(PlayerId player, uint64_t now_ms, const BotPlan& plan = {});

} // namespace net
