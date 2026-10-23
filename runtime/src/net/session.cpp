#include "net/session.h"

#include <utility>

namespace net {

HostSession::HostSession(Draw draw) : draw_(std::move(draw)) {}

bool HostSession::connected(PlayerId p) const {
    return valid(p) && connected_[p];
}

void HostSession::connect(PlayerId p, uint64_t now_ms) {
    if (!valid(p)) return;
    connected_[p] = true;
    // The absent host coming back takes the game again, whatever the phase (pause, vote or draw).
    if (phase_ != Phase::Running && p == host_) restore_host(now_ms);
}

void HostSession::disconnect(PlayerId p, uint64_t now_ms) {
    if (!valid(p) || !connected_[p]) return;
    connected_[p] = false;
    switch (phase_) {
    case Phase::Running:
        if (p == host_) pause(now_ms);
        break;
    case Phase::Paused:
        break;
    case Phase::TakeoverVote:
        volunteer_[p] = false; // a player who left no longer counts as a volunteer
        break;
    case Phase::AwaitingAck:
        if (p == winner_) {
            volunteer_[p] = false;
            const auto left = live_volunteers();
            if (left.empty()) fail_vote(now_ms);
            else draw_winner(now_ms, left);
        }
        break;
    }
}

void HostSession::vote(PlayerId p, bool wants_host, uint64_t) {
    if (phase_ != Phase::TakeoverVote || !valid(p) || !eligible_[p] || !connected_[p]) return;
    volunteer_[p] = wants_host;
}

void HostSession::ack_host(PlayerId p, uint64_t) {
    if (phase_ != Phase::AwaitingAck || p != winner_) return;
    host_ = p;
    clear_vote();
    phase_ = Phase::Running;
    notices_.push_back({NoticeKind::HostChanged, host_, {}});
}

void HostSession::tick(uint64_t now_ms) {
    switch (phase_) {
    case Phase::Running:
        break;
    case Phase::Paused: {
        if (now_ms < pause_since_ms_ + kPauseBeforeTakeoverMs) break;
        bool anyone = false;
        for (PlayerId p = kFirstPlayer; p <= kMaxPlayers; ++p)
            anyone = anyone || (p != host_ && connected_[p]);
        // With nobody else connected, the vote opens as soon as someone connects.
        if (anyone) open_vote(now_ms);
        break;
    }
    case Phase::TakeoverVote:
        if (now_ms >= deadline_ms_) resolve_vote(now_ms);
        break;
    case Phase::AwaitingAck:
        if (now_ms >= deadline_ms_) {
            // The drawn player did not confirm in time: draw again among the others.
            volunteer_[winner_] = false;
            const auto left = live_volunteers();
            if (left.empty()) fail_vote(now_ms);
            else draw_winner(now_ms, left);
        }
        break;
    }
}

std::vector<Notice> HostSession::take_notices() {
    std::vector<Notice> out;
    out.swap(notices_);
    return out;
}

void HostSession::pause(uint64_t now_ms) {
    clear_vote();
    phase_ = Phase::Paused;
    pause_since_ms_ = now_ms;
    notices_.push_back({NoticeKind::PauseStarted, host_, {}});
}

void HostSession::fail_vote(uint64_t now_ms) {
    clear_vote();
    phase_ = Phase::Paused;
    pause_since_ms_ = now_ms; // the next offer comes 5 minutes after this
    notices_.push_back({NoticeKind::VoteFailed, host_, {}});
}

void HostSession::open_vote(uint64_t now_ms) {
    clear_vote();
    phase_ = Phase::TakeoverVote;
    deadline_ms_ = now_ms + kTakeoverVoteMs;
    std::vector<PlayerId> eligible;
    for (PlayerId p = kFirstPlayer; p <= kMaxPlayers; ++p) {
        if (p != host_ && connected_[p]) {
            eligible_[p] = true;
            eligible.push_back(p);
        }
    }
    notices_.push_back({NoticeKind::TakeoverOpened, host_, eligible});
}

void HostSession::resolve_vote(uint64_t now_ms) {
    const auto candidates = live_volunteers();
    if (candidates.empty()) fail_vote(now_ms);
    else draw_winner(now_ms, candidates);
}

void HostSession::draw_winner(uint64_t now_ms, const std::vector<PlayerId>& candidates) {
    size_t index = 0;
    if (candidates.size() > 1 && draw_) {
        index = draw_(candidates.size());
        if (index >= candidates.size()) index = candidates.size() - 1;
    }
    winner_ = candidates[index];
    phase_ = Phase::AwaitingAck;
    deadline_ms_ = now_ms + kHostAckTimeoutMs;
    notices_.push_back({NoticeKind::VoteDrawn, winner_, candidates});
}

void HostSession::restore_host(uint64_t) {
    clear_vote();
    phase_ = Phase::Running;
    notices_.push_back({NoticeKind::HostRestored, host_, {}});
}

void HostSession::clear_vote() {
    eligible_.fill(false);
    volunteer_.fill(false);
    winner_ = 0;
    deadline_ms_ = 0;
}

std::vector<PlayerId> HostSession::live_volunteers() const {
    std::vector<PlayerId> out;
    for (PlayerId p = kFirstPlayer; p <= kMaxPlayers; ++p) {
        if (eligible_[p] && volunteer_[p] && connected_[p]) out.push_back(p);
    }
    return out;
}

} // namespace net
