// Tests for the movement bot (runtime/src/net/bot.h): the schedule itself, and four bots sending their
// state to each other over the ENet mesh. Needs ENet (WWHD_MULTIPLAYER). Returns non-zero on failure.
#include <unistd.h>

#include <cstdio>
#include <vector>

#include "net/bot.h"
#include "net/enet_link.h"
#include "net/wire.h"

namespace {

using net::EnetLink;
using net::PlayerId;
using net::bot_state;

int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

bool same(const net::wire::PlayerState& a, const net::wire::PlayerState& b) {
    return a.same_as(b) && a.yaw == b.yaw;
}

void test_schedule() {
    std::fprintf(stderr, "bot walks forward, walks back, then stands still, every 15 seconds\n");
    CHECK(bot_state(1, 0).z == 0 && bot_state(1, 0).anim == net::kAnimWalkForward);
    CHECK(bot_state(1, 1500).anim == net::kAnimWalkForward && bot_state(1, 1500).z == 450);
    CHECK(bot_state(1, 3000).anim == net::kAnimWalkBack && bot_state(1, 3000).z == 900);
    CHECK(bot_state(1, 4500).z == 450 && bot_state(1, 4500).anim == net::kAnimWalkBack);
    CHECK(bot_state(1, 6000).z == 0 && bot_state(1, 6000).anim == net::kAnimIdle);
    CHECK(bot_state(1, 10000).z == 0 && bot_state(1, 10000).anim == net::kAnimIdle);
    // The next walk starts exactly 15 seconds after the first one.
    CHECK(same(bot_state(1, 1500), bot_state(1, 16500)));
    CHECK(same(bot_state(1, 0), bot_state(1, 30000)));
}

void test_four_separate_starts() {
    std::fprintf(stderr, "four bots start apart from each other\n");
    for (PlayerId p = 1; p <= 4; ++p) {
        CHECK(bot_state(p, 0).x == static_cast<int32_t>(p - 1) * 400);
    }
    for (PlayerId p = 1; p <= 4; ++p) {
        for (PlayerId q = p + 1; q <= 4; ++q) {
            CHECK(bot_state(p, 2000).x != bot_state(q, 2000).x);
        }
    }
}

uint16_t port_for(PlayerId p) {
    return static_cast<uint16_t>(47700 + (getpid() % 150) * 8 + p);
}

void test_mesh_of_bots() {
    std::fprintf(stderr, "four bots exchange their states over the ENet mesh\n");
    EnetLink links[5];
    for (PlayerId p = 1; p <= 4; ++p) CHECK(links[p].open(p, port_for(p)));
    for (PlayerId p = 2; p <= 4; ++p) {
        for (PlayerId q = 1; q < p; ++q) CHECK(links[p].connect_to({q, "127.0.0.1", port_for(q)}));
    }
    // Wait until every link has every other player.
    for (int r = 0; r < 400; ++r) {
        bool all = true;
        for (PlayerId p = 1; p <= 4; ++p) {
            for (PlayerId q = 1; q <= 4; ++q) {
                if (p != q && !links[p].joined(q)) all = false;
            }
        }
        if (all) break;
        for (PlayerId p = 1; p <= 4; ++p) links[p].service(5);
    }
    for (PlayerId p = 1; p <= 4; ++p) {
        for (PlayerId q = 1; q <= 4; ++q) {
            if (p != q) CHECK(links[p].joined(q));
        }
    }

    // Each bot sends its state every 100 ms of game time for 20 seconds.
    const uint64_t last_t = 19900;
    net::wire::PlayerState last[5][5]{};
    bool seen[5][5]{};
    auto drain = [&](PlayerId p) {
        net::Inbound in;
        while (links[p].poll(in)) {
            auto frame = net::wire::decode(in.bytes);
            if (!frame || frame->type != net::wire::Type::PlayerState) continue;
            last[p][in.from] = frame->state;
            seen[p][in.from] = true;
        }
    };
    for (uint64_t t = 0; t <= last_t; t += 100) {
        for (PlayerId p = 1; p <= 4; ++p) {
            net::wire::Frame f;
            f.type = net::wire::Type::PlayerState;
            f.from = p;
            f.state = bot_state(p, t);
            links[p].broadcast(net::wire::encode(f), false);
        }
        for (PlayerId p = 1; p <= 4; ++p) links[p].service(2);
        for (PlayerId p = 1; p <= 4; ++p) drain(p);
    }
    for (int r = 0; r < 20; ++r) {
        for (PlayerId p = 1; p <= 4; ++p) links[p].service(5);
        for (PlayerId p = 1; p <= 4; ++p) drain(p);
    }

    for (PlayerId p = 1; p <= 4; ++p) {
        for (PlayerId q = 1; q <= 4; ++q) {
            if (p == q) continue;
            // Unreliable packets may be lost, so the last state received must match one of the final
            // states the bot sent (the last half second of the schedule).
            CHECK(seen[p][q]);
            bool matches = false;
            for (uint64_t t = last_t - 400; t <= last_t; t += 100) matches = matches || same(last[p][q], bot_state(q, t));
            CHECK(matches);
        }
    }
    for (PlayerId p = 1; p <= 4; ++p) links[p].shutdown();
}

} // namespace

int main() {
    test_schedule();
    test_four_separate_starts();
    test_mesh_of_bots();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all bot checks passed\n");
    return 0;
}
