// Tests for the ENet mesh (runtime/src/net/enet_link.h): three players on localhost, each listening
// and connected to the others. Needs the ENet library (WWHD_MULTIPLAYER). Returns non-zero on failure.
#include <unistd.h>

#include <cstdio>
#include <initializer_list>
#include <vector>

#include "net/enet_link.h"
#include "net/wire.h"

namespace {

using net::EnetLink;
using net::Inbound;
using net::LinkEvent;
using net::LinkEventKind;
using net::PeerAddress;
using net::PlayerId;
using net::wire::Frame;
using net::wire::Type;

int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

void pump(std::initializer_list<EnetLink*> links, int rounds = 20) {
    for (int r = 0; r < rounds; ++r)
        for (EnetLink* l : links) l->service(5);
}

// Ports for this run: different per test and per process, so runs do not collide.
uint16_t port_for(int test) {
    return static_cast<uint16_t>(47100 + (getpid() % 300) * 8 + test);
}

// Takes every pending event of a link, so several checks can look at the same batch.
std::vector<LinkEvent> drain(EnetLink& l) {
    std::vector<LinkEvent> out;
    LinkEvent ev{};
    while (l.poll_event(ev)) out.push_back(ev);
    return out;
}

bool contains(const std::vector<LinkEvent>& events, LinkEventKind kind, PlayerId player) {
    for (const auto& ev : events)
        if (ev.kind == kind && ev.player == player) return true;
    return false;
}

bool receive(EnetLink& l, Inbound& out, std::initializer_list<EnetLink*> all, int rounds = 200) {
    for (int r = 0; r < rounds; ++r) {
        if (l.poll(out)) return true;
        pump(all, 1);
    }
    return false;
}

void test_three_player_mesh() {
    std::fprintf(stderr, "three players form a full mesh\n");
    const uint16_t p1 = port_for(1), p2 = port_for(2), p3 = port_for(3);
    EnetLink a, b, c;
    CHECK(a.open(1, p1));
    CHECK(b.open(2, p2));
    CHECK(c.open(3, p3));

    // Joining order: 2 connects to 1, then 3 connects to 1 and 2 (the roster it got from the lobby).
    CHECK(b.connect_to({1, "127.0.0.1", p1}));
    pump({&a, &b, &c}, 60);
    CHECK(c.connect_to({1, "127.0.0.1", p1}));
    CHECK(c.connect_to({2, "127.0.0.1", p2}));
    pump({&a, &b, &c}, 100);

    CHECK(a.joined(2) && a.joined(3));
    CHECK(b.joined(1) && b.joined(3));
    CHECK(c.joined(1) && c.joined(2));
    const auto a_events = drain(a);
    CHECK(contains(a_events, LinkEventKind::Connected, 2));
    CHECK(contains(a_events, LinkEventKind::Connected, 3));

    a.shutdown();
    b.shutdown();
    c.shutdown();
}

void test_messages_between_all() {
    std::fprintf(stderr, "messages reach one player or all of them\n");
    const uint16_t p1 = port_for(4), p2 = port_for(5), p3 = port_for(6);
    EnetLink a, b, c;
    CHECK(a.open(1, p1) && b.open(2, p2) && c.open(3, p3));
    CHECK(b.connect_to({1, "127.0.0.1", p1}));
    pump({&a, &b, &c}, 60);
    CHECK(c.connect_to({1, "127.0.0.1", p1}));
    CHECK(c.connect_to({2, "127.0.0.1", p2}));
    pump({&a, &b, &c}, 100);

    // Host sends to everyone else: both receive it, and the host itself does not.
    Frame state;
    state.type = Type::PlayerState;
    state.from = 1;
    state.state.x = -42;
    a.broadcast(net::wire::encode(state), true);
    Inbound in;
    CHECK(receive(b, in, {&a, &b, &c}) && in.from == 1);
    auto got = net::wire::decode(in.bytes);
    CHECK(got.has_value() && got->state.x == -42);
    CHECK(receive(c, in, {&a, &b, &c}) && in.from == 1);
    CHECK(!a.poll(in));

    // Player 2 sends straight to player 3, unreliably: the sender id is the connection's player.
    Frame vote;
    vote.type = Type::Vote;
    vote.from = 2;
    vote.wants_host = true;
    b.send(3, net::wire::encode(vote), false);
    CHECK(receive(c, in, {&a, &b, &c}) && in.from == 2);
    auto v = net::wire::decode(in.bytes);
    CHECK(v.has_value() && v->type == Type::Vote && v->wants_host);

    a.shutdown();
    b.shutdown();
    c.shutdown();
}

void test_leave_is_seen_by_the_others() {
    std::fprintf(stderr, "a player leaving is seen by the others, the rest stay connected\n");
    const uint16_t p1 = port_for(7), p2 = port_for(8), p3 = port_for(9);
    EnetLink a, b, c;
    CHECK(a.open(1, p1) && b.open(2, p2) && c.open(3, p3));
    CHECK(b.connect_to({1, "127.0.0.1", p1}));
    pump({&a, &b, &c}, 60);
    CHECK(c.connect_to({1, "127.0.0.1", p1}));
    CHECK(c.connect_to({2, "127.0.0.1", p2}));
    pump({&a, &b, &c}, 100);
    drain(a);
    drain(b);

    c.shutdown();
    pump({&a, &b}, 100);
    CHECK(contains(drain(a), LinkEventKind::Disconnected, 3));
    CHECK(contains(drain(b), LinkEventKind::Disconnected, 3));
    CHECK(!a.joined(3) && !b.joined(3));
    CHECK(a.joined(2) && b.joined(1)); // the others are untouched

    a.shutdown();
    b.shutdown();
}

void test_invalid_connections_refused() {
    std::fprintf(stderr, "connections to yourself, to a bad id, or twice are refused\n");
    const uint16_t p1 = port_for(10), p2 = port_for(11);
    EnetLink a, b;
    CHECK(a.open(1, p1));
    CHECK(!a.connect_to({1, "127.0.0.1", p1}));  // yourself
    CHECK(!a.connect_to({0, "127.0.0.1", p1}));  // no such player
    CHECK(!a.connect_to({5, "127.0.0.1", p1}));  // no such player
    CHECK(b.open(2, p2));
    CHECK(b.connect_to({1, "127.0.0.1", p1}));
    CHECK(!b.connect_to({1, "127.0.0.1", p1}));  // already connecting to 1
    a.shutdown();
    b.shutdown();
}

} // namespace

int main() {
    test_three_player_mesh();
    test_messages_between_all();
    test_leave_is_seen_by_the_others();
    test_invalid_connections_refused();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all enet mesh checks passed\n");
    return 0;
}
