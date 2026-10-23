// Tests for the message format (runtime/src/net/wire.h) and the in-memory transport (net/loopback.h).
// Plain asserts, no framework: returns non-zero when a check fails.
#include <cstdio>
#include <vector>

#include "net/loopback.h"
#include "net/session.h"
#include "net/wire.h"

namespace {

using net::LoopbackHub;
using net::Notice;
using net::NoticeKind;
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

using Bytes = std::vector<uint8_t>;

bool same_notice(const Notice& a, const Notice& b) {
    return a.kind == b.kind && a.player == b.player && a.candidates == b.candidates;
}

// Round trip: encode, then decode, and the frame must come back unchanged.
Frame round_trip(const Frame& f) {
    auto back = net::wire::decode(net::wire::encode(f));
    CHECK(back.has_value());
    return back.value_or(Frame{});
}

void test_round_trip_all_types() {
    std::fprintf(stderr, "round trip of every message type\n");
    Frame hello;
    hello.type = Type::Hello;
    hello.from = 2;
    CHECK(round_trip(hello).type == Type::Hello && round_trip(hello).from == 2);

    Frame disc;
    disc.type = Type::Disconnect;
    disc.from = 4;
    CHECK(round_trip(disc).type == Type::Disconnect);

    Frame ack;
    ack.type = Type::HostAck;
    ack.from = 3;
    CHECK(round_trip(ack).type == Type::HostAck);

    for (bool wants : {false, true}) {
        Frame vote;
        vote.type = Type::Vote;
        vote.from = 2;
        vote.wants_host = wants;
        CHECK(round_trip(vote).wants_host == wants);
    }

    Frame state;
    state.type = Type::PlayerState;
    state.from = 1;
    state.state.x = -350000;
    state.state.y = 123456;
    state.state.z = -1;
    state.state.yaw = -32768;
    state.state.anim = 65535;
    state.state.scene = 7;
    state.state.room = 63;
    CHECK(round_trip(state).state.same_as(state.state));

    Frame welcome;
    welcome.type = Type::Welcome;
    welcome.from = 1;
    welcome.assigned = 3;
    CHECK(round_trip(welcome).assigned == 3);
}

void test_round_trip_notices() {
    std::fprintf(stderr, "every session notice survives the wire\n");
    const NoticeKind kinds[] = {NoticeKind::PauseStarted, NoticeKind::TakeoverOpened, NoticeKind::VoteDrawn,
                                NoticeKind::HostChanged,  NoticeKind::HostRestored,   NoticeKind::VoteFailed};
    for (NoticeKind k : kinds) {
        Notice n{k, 2, {2, 3, 4}};
        Frame f = net::wire::frame_from_notice(n, 1);
        Frame back = round_trip(f);
        CHECK(back.type == Type::Notice);
        CHECK(back.from == 1);
        CHECK(same_notice(back.notice, n));
    }
    Notice none{NoticeKind::PauseStarted, 1, {}};
    CHECK(same_notice(round_trip(net::wire::frame_from_notice(none, 1)).notice, none));
}

void test_rejects_bad_headers() {
    std::fprintf(stderr, "bad headers are rejected\n");
    CHECK(!net::wire::decode({}).has_value());
    CHECK(!net::wire::decode({1, 1, 1, 0}).has_value()); // shorter than the header

    Frame hello;
    hello.type = Type::Hello;
    hello.from = 1;
    const Bytes good = net::wire::encode(hello);

    Bytes wrong_version = good;
    wrong_version[0] = 2;
    CHECK(!net::wire::decode(wrong_version).has_value());

    Bytes type_zero = good;
    type_zero[1] = 0;
    CHECK(!net::wire::decode(type_zero).has_value());

    Bytes type_unknown = good;
    type_unknown[1] = 7;
    CHECK(!net::wire::decode(type_unknown).has_value());

    Bytes from_zero = good;
    from_zero[2] = 0;
    CHECK(!net::wire::decode(from_zero).has_value());

    Bytes from_five = good;
    from_five[2] = 5;
    CHECK(!net::wire::decode(from_five).has_value());

    Bytes extra = good;
    extra.push_back(0); // length in the header says no payload
    CHECK(!net::wire::decode(extra).has_value());

    Bytes hello_with_payload = good;
    hello_with_payload[3] = 1;
    hello_with_payload.push_back(0);
    CHECK(!net::wire::decode(hello_with_payload).has_value());
}

void test_rejects_bad_payloads() {
    std::fprintf(stderr, "bad payloads are rejected\n");
    // Vote: exactly one byte, 0 or 1. Header: version, type, from, length (2 bytes LE).
    CHECK(net::wire::decode({1, 3, 2, 1, 0, 1}).has_value());        // valid: wants host
    CHECK(!net::wire::decode({1, 3, 2, 1, 0, 2}).has_value());       // value 2 is not a bool
    CHECK(!net::wire::decode({1, 3, 2, 2, 0, 1, 0}).has_value());    // length 2 for a 1-byte payload

    // Notice payload: kind, player, count, candidates.
    CHECK(net::wire::decode({1, 5, 1, 3, 0, 0, 1, 0}).has_value());  // valid: kind 0, player 1, no candidates
    CHECK(!net::wire::decode({1, 5, 1, 3, 0, 6, 1, 0}).has_value()); // kind 6 is out of range
    CHECK(!net::wire::decode({1, 5, 1, 3, 0, 0, 1, 5}).has_value()); // count 5 is more than 4 players
    CHECK(!net::wire::decode({1, 5, 1, 3, 0, 0, 1, 1}).has_value()); // count 1 but no candidate byte
    CHECK(!net::wire::decode({1, 5, 1, 4, 0, 0, 1, 1, 0}).has_value()); // candidate 0 is not a player
    CHECK(!net::wire::decode({1, 5, 1, 3, 0, 0, 0, 0}).has_value()); // player 0 is not a player

    // Welcome: one byte, a joining player id (2..4), never the host's own id 1.
    CHECK(net::wire::decode({1, 7, 1, 1, 0, 2}).has_value());
    CHECK(!net::wire::decode({1, 7, 1, 1, 0, 1}).has_value());
    CHECK(!net::wire::decode({1, 7, 1, 1, 0, 5}).has_value());
    CHECK(!net::wire::decode({1, 7, 1, 0, 0}).has_value());

    // PlayerState: exactly 18 bytes.
    Bytes state(5 + 17, 0);
    state[0] = 1;
    state[1] = 6;
    state[2] = 1;
    state[3] = 17;
    CHECK(!net::wire::decode(state).has_value());
}

void test_loopback() {
    std::fprintf(stderr, "in-memory transport: order, joins and leaves\n");
    LoopbackHub hub;
    for (PlayerId p = 1; p <= 4; ++p) hub.join(p);
    CHECK(hub.joined(4));

    hub.send(1, 2, {10});
    hub.send(3, 2, {20});
    PlayerId from = 0;
    Bytes bytes;
    CHECK(hub.receive(2, from, bytes) && from == 1 && bytes == Bytes{10});
    CHECK(hub.receive(2, from, bytes) && from == 3 && bytes == Bytes{20});
    CHECK(!hub.receive(2, from, bytes));

    // A player who leaves loses their queued messages and gets nothing new.
    hub.send(1, 4, {30});
    hub.leave(4);
    CHECK(!hub.joined(4));
    CHECK(!hub.receive(4, from, bytes));
    hub.send(1, 4, {31});
    CHECK(!hub.receive(4, from, bytes));

    // Messages from or to a player who never joined are dropped.
    hub.send(1, 5, {40});
    hub.send(0, 2, {41});
    CHECK(!hub.receive(2, from, bytes));
}

void test_session_over_the_wire() {
    std::fprintf(stderr, "a host leaving, relayed as frames, reaches the other players\n");
    LoopbackHub hub;
    for (PlayerId p = 1; p <= 4; ++p) hub.join(p);

    // The host's side: player 1 leaves, the server sends a Disconnect frame for them to everyone.
    Frame leave;
    leave.type = Type::Disconnect;
    leave.from = 1;
    for (PlayerId to = 2; to <= 4; ++to) hub.send(1, to, net::wire::encode(leave));

    // Player 3's side: decode what arrives and apply it to a local session copy.
    net::HostSession session([](size_t) { return size_t{0}; });
    for (PlayerId p = 1; p <= 4; ++p) session.connect(p, 0);

    PlayerId from = 0;
    Bytes bytes;
    CHECK(hub.receive(3, from, bytes));
    auto frame = net::wire::decode(bytes);
    CHECK(frame.has_value() && frame->type == Type::Disconnect && frame->from == 1);
    if (frame) session.disconnect(frame->from, 0);
    CHECK(session.paused());

    // The pause notice goes back out over the wire as a Notice frame.
    auto notices = session.take_notices();
    CHECK(notices.size() == 1 && notices[0].kind == NoticeKind::PauseStarted);
    for (const auto& n : notices) {
        Frame out = net::wire::frame_from_notice(n, 2);
        hub.send(2, 4, net::wire::encode(out));
    }
    // Player 4 also got the Disconnect frame first: drain it so the Notice is the next message.
    CHECK(hub.receive(4, from, bytes));
    CHECK(net::wire::decode(bytes).has_value() && net::wire::decode(bytes)->type == Type::Disconnect);
    CHECK(hub.receive(4, from, bytes));
    auto relayed = net::wire::decode(bytes);
    CHECK(relayed.has_value() && relayed->type == Type::Notice);
    if (relayed) CHECK(same_notice(relayed->notice, notices[0]));
}

} // namespace

int main() {
    test_round_trip_all_types();
    test_round_trip_notices();
    test_rejects_bad_headers();
    test_rejects_bad_payloads();
    test_loopback();
    test_session_over_the_wire();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all net wire checks passed\n");
    return 0;
}
