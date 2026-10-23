// Tests for the ENet transport (runtime/src/net/enet_link.h): a host and clients on localhost.
// Needs the ENet library (WWHD_MULTIPLAYER). Plain asserts; returns non-zero when a check fails.
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

// Services every link a few times so packets and handshakes move on.
void pump(std::initializer_list<EnetLink*> links, int rounds = 20) {
    for (int r = 0; r < rounds; ++r)
        for (EnetLink* l : links) l->service(5);
}

bool wait_for_self(EnetLink& client, std::initializer_list<EnetLink*> all, int rounds = 400) {
    for (int r = 0; r < rounds && client.self() == 0; ++r) pump(all, 1);
    return client.self() != 0;
}

bool next_event(EnetLink& l, LinkEvent& out) {
    return l.poll_event(out);
}

bool receive(EnetLink& l, Inbound& out, std::initializer_list<EnetLink*> all, int rounds = 100) {
    for (int r = 0; r < rounds; ++r) {
        if (l.poll(out)) return true;
        pump(all, 1);
    }
    return false;
}

Frame hello(PlayerId from) {
    Frame f;
    f.type = Type::Hello;
    f.from = from;
    return f;
}

void test_join_assigns_ids() {
    std::fprintf(stderr, "clients join and receive ids 2 and 3\n");
    const uint16_t port = static_cast<uint16_t>(47000 + (getpid() % 1000));
    EnetLink host;
    CHECK(host.host(port));
    CHECK(host.self() == 1 && host.is_host());

    EnetLink c2, c3;
    CHECK(c2.connect("127.0.0.1", port));
    CHECK(wait_for_self(c2, {&host, &c2}));
    CHECK(c2.self() == 2);

    LinkEvent ev{};
    CHECK(next_event(host, ev) && ev.kind == LinkEventKind::Connected && ev.player == 2);
    CHECK(next_event(c2, ev) && ev.kind == LinkEventKind::Connected && ev.player == 2);

    CHECK(c3.connect("127.0.0.1", port));
    CHECK(wait_for_self(c3, {&host, &c2, &c3}));
    CHECK(c3.self() == 3);
    CHECK(next_event(host, ev) && ev.kind == LinkEventKind::Connected && ev.player == 3);

    CHECK(host.joined(2) && host.joined(3) && !host.joined(4));
    CHECK(c2.joined(1) && c2.joined(2));

    c2.shutdown();
    c3.shutdown();
    host.shutdown();
}

void test_messages_both_ways() {
    std::fprintf(stderr, "messages go client to host and host to clients\n");
    const uint16_t port = static_cast<uint16_t>(47000 + (getpid() % 1000) + 1);
    EnetLink host, c2, c3;
    CHECK(host.host(port));
    CHECK(c2.connect("127.0.0.1", port));
    CHECK(wait_for_self(c2, {&host, &c2}));
    CHECK(c3.connect("127.0.0.1", port));
    CHECK(wait_for_self(c3, {&host, &c2, &c3}));

    // Client 2 -> host, reliable: the host sees who sent it.
    c2.send(1, net::wire::encode(hello(2)), true);
    Inbound in;
    CHECK(receive(host, in, {&host, &c2, &c3}));
    CHECK(in.from == 2);
    auto f = net::wire::decode(in.bytes);
    CHECK(f.has_value() && f->type == Type::Hello && f->from == 2);

    // Host -> everyone: each client gets the same bytes, and nothing is looped back to the host.
    Frame state;
    state.type = Type::PlayerState;
    state.from = 1;
    state.state.x = -42;
    state.state.anim = 7;
    host.broadcast(net::wire::encode(state), true);
    CHECK(receive(c2, in, {&host, &c2, &c3}));
    auto got2 = net::wire::decode(in.bytes);
    CHECK(got2.has_value() && got2->type == Type::PlayerState && got2->state.x == -42);
    CHECK(receive(c3, in, {&host, &c2, &c3}));
    auto got3 = net::wire::decode(in.bytes);
    CHECK(got3.has_value() && got3->state.anim == 7);
    CHECK(!host.poll(in));

    // Unreliable messages on localhost arrive too.
    c3.send(1, net::wire::encode(hello(3)), false);
    CHECK(receive(host, in, {&host, &c2, &c3}));
    CHECK(in.from == 3);

    c2.shutdown();
    c3.shutdown();
    host.shutdown();
}

void test_leave_is_reported() {
    std::fprintf(stderr, "a client leaving is reported to the host\n");
    const uint16_t port = static_cast<uint16_t>(47000 + (getpid() % 1000) + 2);
    EnetLink host, c2;
    CHECK(host.host(port));
    CHECK(c2.connect("127.0.0.1", port));
    CHECK(wait_for_self(c2, {&host, &c2}));

    LinkEvent ev{};
    while (next_event(host, ev)) {
    }

    c2.shutdown();
    for (int r = 0; r < 200 && host.joined(2); ++r) pump({&host}, 1);
    CHECK(!host.joined(2));
    CHECK(next_event(host, ev) && ev.kind == LinkEventKind::Disconnected && ev.player == 2);

    host.shutdown();
}

void test_full_host_refuses_extra_client() {
    std::fprintf(stderr, "a fifth player is refused: the host holds three clients at most\n");
    const uint16_t port = static_cast<uint16_t>(47000 + (getpid() % 1000) + 3);
    EnetLink host, c2, c3, c4, c5;
    CHECK(host.host(port));
    CHECK(c2.connect("127.0.0.1", port));
    CHECK(wait_for_self(c2, {&host, &c2}));
    CHECK(c3.connect("127.0.0.1", port));
    CHECK(wait_for_self(c3, {&host, &c2, &c3}));
    CHECK(c4.connect("127.0.0.1", port));
    CHECK(wait_for_self(c4, {&host, &c2, &c3, &c4}));
    CHECK(c4.self() == 4);

    CHECK(c5.connect("127.0.0.1", port));
    for (int r = 0; r < 200 && c5.self() == 0; ++r) pump({&host, &c2, &c3, &c4, &c5}, 1);
    CHECK(c5.self() == 0); // no id given: the host turned it away

    c5.shutdown();
    c4.shutdown();
    c3.shutdown();
    c2.shutdown();
    host.shutdown();
}

} // namespace

int main() {
    test_join_assigns_ids();
    test_messages_both_ways();
    test_leave_is_reported();
    test_full_host_refuses_extra_client();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all enet transport checks passed\n");
    return 0;
}
