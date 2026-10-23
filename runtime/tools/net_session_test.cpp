// Tests for the host session state machine (runtime/src/net/session.h).
// Plain asserts, no framework: returns non-zero when a check fails.
#include "net/session.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using net::HostSession;
using net::Notice;
using net::NoticeKind;
using net::Phase;
using net::PlayerId;

constexpr uint64_t S = 1000;
constexpr uint64_t M = 60 * S;

int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// Scripted random source: returns the queued picks in order (0 when empty) and records each call.
struct ScriptedDraw {
    std::vector<size_t> picks;
    std::vector<size_t> calls; // counts passed to the draw
    HostSession::Draw fn() {
        return [this](size_t count) {
            calls.push_back(count);
            size_t index = 0;
            if (!picks.empty()) {
                index = picks.front();
                picks.erase(picks.begin());
            }
            return index;
        };
    }
};

bool has(const std::vector<Notice>& notices, NoticeKind kind) {
    for (const auto& n : notices)
        if (n.kind == kind) return true;
    return false;
}

const Notice* find(const std::vector<Notice>& notices, NoticeKind kind) {
    for (const auto& n : notices)
        if (n.kind == kind) return &n;
    return nullptr;
}

std::vector<PlayerId> candidates_of(const std::vector<Notice>& notices, NoticeKind kind) {
    const Notice* n = find(notices, kind);
    return n ? n->candidates : std::vector<PlayerId>{};
}

// Puts players 1..count in the session, host (1) first.
void join_all(HostSession& s, int count, uint64_t t) {
    for (PlayerId p = 1; p <= count; ++p) s.connect(p, t);
}

// Host 1 leaves at t, then the vote opens at t + 5 minutes.
void leave_and_open_vote(HostSession& s, uint64_t t) {
    s.disconnect(1, t);
    s.tick(t + 5 * M);
}

void test_single_volunteer_takes_over() {
    std::fprintf(stderr, "single volunteer takes over\n");
    ScriptedDraw rng;
    HostSession s(rng.fn());
    join_all(s, 3, 0);
    const uint64_t t = 10 * S;
    s.disconnect(1, t);
    CHECK(s.phase() == Phase::Paused);
    CHECK(has(s.take_notices(), NoticeKind::PauseStarted));

    s.tick(t + 5 * M - 1);
    CHECK(s.phase() == Phase::Paused);

    s.tick(t + 5 * M);
    CHECK(s.phase() == Phase::TakeoverVote);
    auto opened = s.take_notices();
    CHECK(candidates_of(opened, NoticeKind::TakeoverOpened) == (std::vector<PlayerId>{2, 3}));

    const uint64_t vote_t = t + 5 * M;
    s.vote(2, true, vote_t);
    s.vote(3, false, vote_t);
    s.tick(vote_t + 30 * S);
    CHECK(s.phase() == Phase::AwaitingAck);
    CHECK(rng.calls.empty()); // one volunteer: no draw
    auto drawn = s.take_notices();
    const Notice* d = find(drawn, NoticeKind::VoteDrawn);
    CHECK(d != nullptr && d->player == 2);

    s.ack_host(3, vote_t + 31 * S); // not the drawn player: ignored
    CHECK(s.phase() == Phase::AwaitingAck);
    s.ack_host(2, vote_t + 32 * S);
    CHECK(s.phase() == Phase::Running);
    CHECK(s.host() == 2);
    CHECK(has(s.take_notices(), NoticeKind::HostChanged));
}

void test_draw_among_several_volunteers() {
    std::fprintf(stderr, "draw among several volunteers\n");
    ScriptedDraw rng;
    rng.picks = {1};
    HostSession s(rng.fn());
    join_all(s, 4, 0);
    leave_and_open_vote(s, 0);
    s.take_notices();
    const uint64_t vote_t = 5 * M;
    s.vote(2, true, vote_t);
    s.vote(3, true, vote_t);
    s.vote(4, true, vote_t);
    s.tick(vote_t + 30 * S);
    CHECK(s.phase() == Phase::AwaitingAck);
    CHECK(rng.calls == (std::vector<size_t>{3}));
    auto n = s.take_notices();
    CHECK(candidates_of(n, NoticeKind::VoteDrawn) == (std::vector<PlayerId>{2, 3, 4}));
    const Notice* d = find(n, NoticeKind::VoteDrawn);
    CHECK(d != nullptr && d->player == 3);
    s.ack_host(3, vote_t + 31 * S);
    CHECK(s.host() == 3);
    CHECK(s.phase() == Phase::Running);
}

void test_no_volunteers_pauses_again() {
    std::fprintf(stderr, "no volunteers: pause, offer again 5 minutes later\n");
    HostSession s([](size_t) { return size_t{0}; });
    join_all(s, 3, 0);
    leave_and_open_vote(s, 0);
    s.take_notices();
    const uint64_t vote_t = 5 * M;
    s.vote(2, false, vote_t);
    s.tick(vote_t + 30 * S);
    CHECK(s.phase() == Phase::Paused);
    CHECK(has(s.take_notices(), NoticeKind::VoteFailed));

    const uint64_t failed_t = vote_t + 30 * S;
    s.tick(failed_t + 5 * M - 1);
    CHECK(s.phase() == Phase::Paused);
    s.tick(failed_t + 5 * M);
    CHECK(s.phase() == Phase::TakeoverVote);
}

void test_host_returns_during_vote() {
    std::fprintf(stderr, "host returns during the vote: vote cancelled, host restored\n");
    ScriptedDraw rng;
    HostSession s(rng.fn());
    join_all(s, 3, 0);
    leave_and_open_vote(s, 0);
    s.take_notices();
    s.vote(2, true, 5 * M);
    s.connect(1, 5 * M + 5 * S);
    CHECK(s.phase() == Phase::Running);
    CHECK(s.host() == 1);
    CHECK(has(s.take_notices(), NoticeKind::HostRestored));
    s.tick(5 * M + 60 * S);
    CHECK(s.phase() == Phase::Running);
    CHECK(rng.calls.empty());
}

void test_host_returns_during_ack() {
    std::fprintf(stderr, "host returns while a draw awaits confirmation\n");
    HostSession s([](size_t) { return size_t{0}; });
    join_all(s, 3, 0);
    leave_and_open_vote(s, 0);
    s.take_notices();
    s.vote(2, true, 5 * M);
    s.tick(5 * M + 30 * S);
    CHECK(s.phase() == Phase::AwaitingAck);
    s.take_notices();
    s.connect(1, 5 * M + 35 * S);
    CHECK(s.phase() == Phase::Running);
    CHECK(s.host() == 1);
    s.ack_host(2, 5 * M + 36 * S); // the draw was cancelled: ignored
    CHECK(s.host() == 1);
    CHECK(s.phase() == Phase::Running);
}

void test_original_host_back_after_takeover_stays_player() {
    std::fprintf(stderr, "original host back after a takeover: stays a player\n");
    HostSession s([](size_t) { return size_t{0}; });
    join_all(s, 3, 0);
    leave_and_open_vote(s, 0);
    s.vote(2, true, 5 * M);
    s.tick(5 * M + 30 * S);
    s.ack_host(2, 5 * M + 31 * S);
    s.take_notices();
    s.connect(1, 10 * M);
    CHECK(s.host() == 2);
    CHECK(s.phase() == Phase::Running);
    CHECK(s.take_notices().empty());
}

void test_winner_leaves_before_ack_redraws() {
    std::fprintf(stderr, "drawn player leaves before confirming: redraw among the others\n");
    ScriptedDraw rng;
    rng.picks = {1, 0}; // first draw picks index 1 (player 3), second index 0
    HostSession s(rng.fn());
    join_all(s, 4, 0);
    leave_and_open_vote(s, 0);
    s.take_notices();
    const uint64_t vote_t = 5 * M;
    s.vote(2, true, vote_t);
    s.vote(3, true, vote_t);
    s.vote(4, true, vote_t);
    s.tick(vote_t + 30 * S);
    CHECK(s.phase() == Phase::AwaitingAck);
    s.take_notices();

    s.disconnect(3, vote_t + 31 * S);
    CHECK(s.phase() == Phase::AwaitingAck);
    CHECK(rng.calls == (std::vector<size_t>{3, 2}));
    auto n = s.take_notices();
    const Notice* d = find(n, NoticeKind::VoteDrawn);
    CHECK(d != nullptr && d->player == 2);
    CHECK(candidates_of(n, NoticeKind::VoteDrawn) == (std::vector<PlayerId>{2, 4}));
    s.ack_host(2, vote_t + 32 * S);
    CHECK(s.host() == 2);
}

void test_winner_leaves_with_no_one_left_pauses() {
    std::fprintf(stderr, "drawn player leaves and no volunteer is left: back to pause\n");
    HostSession s([](size_t) { return size_t{0}; });
    join_all(s, 3, 0);
    leave_and_open_vote(s, 0);
    s.take_notices();
    s.vote(2, true, 5 * M);
    s.tick(5 * M + 30 * S);
    s.take_notices();
    s.disconnect(2, 5 * M + 31 * S);
    CHECK(s.phase() == Phase::Paused);
    CHECK(has(s.take_notices(), NoticeKind::VoteFailed));
}

void test_unconfirmed_draw_times_out() {
    std::fprintf(stderr, "drawn player does not confirm in time: draw again\n");
    ScriptedDraw rng;
    rng.picks = {0};
    HostSession s(rng.fn());
    join_all(s, 4, 0);
    leave_and_open_vote(s, 0);
    s.take_notices();
    const uint64_t vote_t = 5 * M;
    s.vote(2, true, vote_t);
    s.vote(3, true, vote_t);
    s.tick(vote_t + 30 * S);
    CHECK(s.phase() == Phase::AwaitingAck);
    auto first = s.take_notices();
    CHECK(find(first, NoticeKind::VoteDrawn)->player == 2);

    s.tick(vote_t + 30 * S + net::kHostAckTimeoutMs - 1);
    CHECK(s.phase() == Phase::AwaitingAck);
    s.tick(vote_t + 30 * S + net::kHostAckTimeoutMs);
    CHECK(s.phase() == Phase::AwaitingAck);
    auto second = s.take_notices();
    const Notice* d = find(second, NoticeKind::VoteDrawn);
    CHECK(d != nullptr && d->player == 3);
    CHECK(rng.calls == (std::vector<size_t>{2})); // the redraw had one candidate left: no second call
    s.ack_host(3, vote_t + 60 * S);
    CHECK(s.host() == 3);
}

void test_only_eligible_players_vote() {
    std::fprintf(stderr, "only players connected when the vote opened can vote\n");
    HostSession s([](size_t) { return size_t{0}; });
    join_all(s, 4, 0);
    s.disconnect(3, 1 * S); // not connected when the vote opens
    s.disconnect(1, 2 * S);
    s.tick(2 * S + 5 * M);
    CHECK(s.phase() == Phase::TakeoverVote);
    auto opened = s.take_notices();
    CHECK(candidates_of(opened, NoticeKind::TakeoverOpened) == (std::vector<PlayerId>{2, 4}));
    s.vote(3, true, 5 * M); // disconnected: ignored
    s.vote(1, true, 5 * M); // the absent host: ignored
    s.vote(4, true, 5 * M);
    s.tick(2 * S + 5 * M + 30 * S);
    auto drawn = s.take_notices(); // keep the vector alive while we look into it
    const Notice* d = find(drawn, NoticeKind::VoteDrawn);
    CHECK(d != nullptr && d->player == 4);
}

void test_volunteer_leaves_mid_vote() {
    std::fprintf(stderr, "a volunteer who leaves mid-vote no longer counts\n");
    HostSession s([](size_t) { return size_t{0}; });
    join_all(s, 3, 0);
    leave_and_open_vote(s, 0);
    s.take_notices();
    s.vote(2, true, 5 * M);
    s.disconnect(2, 5 * M + 10 * S);
    s.tick(5 * M + 30 * S);
    CHECK(s.phase() == Phase::Paused);
    CHECK(has(s.take_notices(), NoticeKind::VoteFailed));
}

void test_new_host_leaves_and_pauses_again() {
    std::fprintf(stderr, "a new host leaving pauses the game again\n");
    HostSession s([](size_t) { return size_t{0}; });
    join_all(s, 4, 0);
    leave_and_open_vote(s, 0);
    s.take_notices();
    s.vote(2, true, 5 * M);
    s.tick(5 * M + 30 * S);
    s.ack_host(2, 5 * M + 31 * S);
    s.take_notices();

    s.disconnect(2, 20 * M);
    CHECK(s.phase() == Phase::Paused);
    CHECK(has(s.take_notices(), NoticeKind::PauseStarted));
    s.tick(20 * M + 5 * M);
    CHECK(s.phase() == Phase::TakeoverVote);
    CHECK(candidates_of(s.take_notices(), NoticeKind::TakeoverOpened) == (std::vector<PlayerId>{3, 4}));
}

void test_invalid_ids_ignored() {
    std::fprintf(stderr, "invalid player IDs are ignored\n");
    HostSession s([](size_t) { return size_t{0}; });
    s.connect(0, 0);
    s.connect(5, 0);
    CHECK(!s.connected(0));
    CHECK(!s.connected(5));
    CHECK(s.phase() == Phase::Running);
}

} // namespace

int main() {
    test_single_volunteer_takes_over();
    test_draw_among_several_volunteers();
    test_no_volunteers_pauses_again();
    test_host_returns_during_vote();
    test_host_returns_during_ack();
    test_original_host_back_after_takeover_stays_player();
    test_winner_leaves_before_ack_redraws();
    test_winner_leaves_with_no_one_left_pauses();
    test_unconfirmed_draw_times_out();
    test_only_eligible_players_vote();
    test_volunteer_leaves_mid_vote();
    test_new_host_leaves_and_pauses_again();
    test_invalid_ids_ignored();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all net session checks passed\n");
    return 0;
}
