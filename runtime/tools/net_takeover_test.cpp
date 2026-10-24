// Tests for the session over the mesh (runtime/src/net/session_net.h): three players on localhost, the
// host leaves, the others pause, vote, draw and resume with the new host. Needs ENet (WWHD_MULTIPLAYER).
// Returns non-zero on failure.
#include <unistd.h>

#include <cstdio>
#include <initializer_list>

#include "net/enet_link.h"
#include "net/session_net.h"

namespace {

using net::EnetLink;
using net::PeerAddress;
using net::Phase;
using net::PlayerId;
using net::SessionNet;

constexpr uint64_t S = 1000;
constexpr uint64_t M = 60 * S;

int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

uint16_t port_for(int test, int player) {
    return static_cast<uint16_t>(47500 + (getpid() % 200) * 8 + test * 4 + player);
}

// A player: a mesh link and the session on top of it. Draws always pick the first candidate.
struct Node {
    EnetLink link;
    SessionNet session;
    Node() : session(link, [](size_t) { return size_t{0}; }) {}
};

// Services the links and ticks the sessions for a number of rounds, at the given game time.
void run(std::initializer_list<Node*> nodes, uint64_t now, int rounds = 40) {
    for (int r = 0; r < rounds; ++r) {
        for (Node* n : nodes) n->link.service(5);
        for (Node* n : nodes) n->session.tick(now);
    }
}

// Players 1, 2 and 3 open their ports, then each newer player connects to the ones already there.
void form_mesh(Node& a, Node& b, Node& c, int test, uint64_t now) {
    CHECK(a.link.open(1, port_for(test, 1)));
    CHECK(b.link.open(2, port_for(test, 2)));
    CHECK(c.link.open(3, port_for(test, 3)));
    CHECK(b.link.connect_to({1, "127.0.0.1", port_for(test, 1)}));
    run({&a, &b, &c}, now, 60);
    CHECK(c.link.connect_to({1, "127.0.0.1", port_for(test, 1)}));
    CHECK(c.link.connect_to({2, "127.0.0.1", port_for(test, 2)}));
    run({&a, &b, &c}, now, 100);
    a.session.start(now);
    b.session.start(now);
    c.session.start(now);
}

void test_takeover_after_host_leaves() {
    std::fprintf(stderr, "host leaves: pause, takeover vote, draw, resume with the new host\n");
    Node a, b, c;
    uint64_t now = 0;
    form_mesh(a, b, c, 1, now);
    CHECK(a.session.is_authority());
    CHECK(a.session.view().phase == Phase::Running);
    CHECK(c.session.view().host == 1 && c.session.view().phase == Phase::Running);

    // The host leaves. The lowest id left (player 2) takes over the session and pauses the game.
    now = 1000;
    a.link.shutdown();
    run({&b, &c}, now);
    CHECK(b.session.is_authority());
    CHECK(!c.session.is_authority());
    CHECK(b.session.view().phase == Phase::Paused);
    CHECK(c.session.view().phase == Phase::Paused);
    CHECK(c.session.view().host == 1);

    // Nothing happens before the 5 minutes are over.
    now = 1000 + 5 * M - S;
    run({&b, &c}, now);
    CHECK(c.session.view().phase == Phase::Paused);

    // The vote opens: player 3 (the other one who can vote) says yes.
    now = 1000 + 5 * M;
    run({&b, &c}, now);
    CHECK(c.session.view().phase == Phase::TakeoverVote);
    c.session.vote(true, now);
    run({&b, &c}, now);

    // After the 30 seconds the single volunteer is drawn, confirms, and becomes the host.
    now = 1000 + 5 * M + 30 * S;
    run({&b, &c}, now);
    CHECK(b.session.view().host == 3 && b.session.view().phase == Phase::Running);
    CHECK(c.session.view().host == 3 && c.session.view().phase == Phase::Running);
    CHECK(b.session.view().winner == 0 && c.session.view().winner == 0);

    b.link.shutdown();
    c.link.shutdown();
}

void test_late_player_gets_the_snapshot() {
    std::fprintf(stderr, "a player who arrives after the takeover catches up from a snapshot\n");
    Node a, b, c, d;
    uint64_t now = 0;
    form_mesh(a, b, c, 2, now);

    now = 1000;
    a.link.shutdown();
    run({&b, &c}, now);
    now = 1000 + 5 * M;
    run({&b, &c}, now);
    c.session.vote(true, now);
    now = 1000 + 5 * M + 30 * S;
    run({&b, &c}, now);
    CHECK(b.session.view().host == 3);

    // Player 4 joins the game as it is now: it should see host 3 and no pause, without any notice
    // having reached it.
    CHECK(d.link.open(4, port_for(2, 4)));
    CHECK(d.link.connect_to({2, "127.0.0.1", port_for(2, 2)}));
    CHECK(d.link.connect_to({3, "127.0.0.1", port_for(2, 3)}));
    run({&b, &c, &d}, now, 120);
    d.session.start(now);
    run({&b, &c, &d}, now, 20);
    CHECK(!d.session.is_authority());
    CHECK(d.session.authority() == 2);
    CHECK(d.session.view().host == 3);
    CHECK(d.session.view().phase == Phase::Running);

    b.link.shutdown();
    c.link.shutdown();
    d.link.shutdown();
}

} // namespace

int main() {
    test_takeover_after_host_leaves();
    test_late_player_gets_the_snapshot();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all takeover checks passed\n");
    return 0;
}
