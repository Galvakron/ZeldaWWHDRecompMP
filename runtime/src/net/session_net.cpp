#include "net/session_net.h"

#include <utility>

#include "net/wire.h"

namespace net {

SessionNet::SessionNet(EnetLink& link, HostSession::Draw draw) : link_(link), draw_(std::move(draw)) {}

void SessionNet::start(uint64_t now_ms) {
    for (PlayerId p = kFirstPlayer; p <= kMaxPlayers; ++p) connected_[p] = link_.joined(p) || p == link_.self();
    authority_ = lowest_connected();
    if (is_authority()) adopt(now_ms);
}

void SessionNet::tick(uint64_t now_ms) {
    LinkEvent ev{};
    while (link_.poll_event(ev)) {
        if (ev.kind == LinkEventKind::Connected) on_connected(ev.player, now_ms);
        else on_disconnected(ev.player, now_ms);
    }
    Inbound in;
    while (link_.poll(in)) on_frame(in, now_ms);
    if (session_) {
        session_->tick(now_ms);
        publish(session_->take_notices(), now_ms);
    }
}

void SessionNet::vote(bool wants_host, uint64_t now_ms) {
    if (is_authority()) {
        if (!session_) return;
        session_->vote(link_.self(), wants_host, now_ms);
        publish(session_->take_notices(), now_ms);
        return;
    }
    wire::Frame f;
    f.type = wire::Type::Vote;
    f.from = link_.self();
    f.wants_host = wants_host;
    link_.send(authority_, wire::encode(f), true);
}

std::vector<Notice> SessionNet::take_events() {
    std::vector<Notice> out;
    out.swap(events_);
    return out;
}

void SessionNet::on_connected(PlayerId p, uint64_t now_ms) {
    connected_[p] = true;
    if (session_) {
        session_->connect(p, now_ms);
        publish(session_->take_notices(), now_ms);
    }
    if (is_authority()) send_snapshot(p);
}

void SessionNet::on_disconnected(PlayerId p, uint64_t now_ms) {
    connected_[p] = false;
    if (session_) {
        session_->disconnect(p, now_ms);
        publish(session_->take_notices(), now_ms);
    }
    if (p == authority_) {
        authority_ = lowest_connected();
        if (is_authority()) adopt(now_ms);
        else release();
    }
}

void SessionNet::on_frame(const Inbound& in, uint64_t now_ms) {
    auto frame = wire::decode(in.bytes);
    if (!frame) return;

    switch (frame->type) {
    case wire::Type::Notice:
        if (is_authority() || in.from != authority_) return;
        publish({frame->notice}, now_ms);
        return;

    case wire::Type::Snapshot: {
        // Only the authority sends snapshots, so a joiner that does not know the authority yet adopts the
        // sender.
        if (is_authority()) return;
        authority_ = in.from;
        const auto& s = frame->snapshot;
        view_.phase = static_cast<Phase>(s.phase);
        view_.host = s.host;
        view_.winner = s.winner;
        view_.candidates = s.candidates;
        if (view_.phase == Phase::AwaitingAck && view_.winner == link_.self()) acknowledge(now_ms);
        return;
    }

    case wire::Type::Vote:
        if (!is_authority() || !session_) return;
        session_->vote(in.from, frame->wants_host, now_ms);
        publish(session_->take_notices(), now_ms);
        return;

    case wire::Type::HostAck:
        if (!is_authority() || !session_) return;
        session_->ack_host(in.from, now_ms);
        publish(session_->take_notices(), now_ms);
        return;

    default:
        return;
    }
}

void SessionNet::adopt(uint64_t now_ms) {
    session_ = std::make_unique<HostSession>(draw_, view_.host);
    for (PlayerId p = kFirstPlayer; p <= kMaxPlayers; ++p) {
        if (connected_[p]) session_->connect(p, now_ms);
    }
    // The game is running for a host who is no longer here: the session starts paused.
    if (!connected_[view_.host]) {
        session_->connect(view_.host, now_ms);
        session_->disconnect(view_.host, now_ms);
    }
    publish(session_->take_notices(), now_ms);
    for (PlayerId p = kFirstPlayer; p <= kMaxPlayers; ++p) {
        if (p != link_.self() && connected_[p]) send_snapshot(p);
    }
}

void SessionNet::release() {
    session_.reset();
}

void SessionNet::publish(std::vector<Notice> notices, uint64_t now_ms) {
    bool drawn_self = false;
    for (const Notice& n : notices) {
        if (is_authority()) link_.broadcast(wire::encode(wire::frame_from_notice(n, link_.self())), true);
        apply(n);
        events_.push_back(n);
        if (n.kind == NoticeKind::VoteDrawn && n.player == link_.self()) drawn_self = true;
    }
    if (drawn_self) acknowledge(now_ms);
}

void SessionNet::apply(const Notice& n) {
    switch (n.kind) {
    case NoticeKind::PauseStarted:
        view_.phase = Phase::Paused;
        view_.host = n.player;
        view_.winner = 0;
        view_.candidates.clear();
        break;
    case NoticeKind::TakeoverOpened:
        view_.phase = Phase::TakeoverVote;
        view_.winner = 0;
        view_.candidates = n.candidates;
        break;
    case NoticeKind::VoteDrawn:
        view_.phase = Phase::AwaitingAck;
        view_.winner = n.player;
        view_.candidates = n.candidates;
        break;
    case NoticeKind::HostChanged:
    case NoticeKind::HostRestored:
        view_.phase = Phase::Running;
        view_.host = n.player;
        view_.winner = 0;
        view_.candidates.clear();
        break;
    case NoticeKind::VoteFailed:
        view_.phase = Phase::Paused;
        view_.winner = 0;
        view_.candidates.clear();
        break;
    }
}

void SessionNet::acknowledge(uint64_t now_ms) {
    if (is_authority()) {
        if (!session_) return;
        session_->ack_host(link_.self(), now_ms);
        publish(session_->take_notices(), now_ms);
        return;
    }
    wire::Frame f;
    f.type = wire::Type::HostAck;
    f.from = link_.self();
    link_.send(authority_, wire::encode(f), true);
}

void SessionNet::send_snapshot(PlayerId to) {
    wire::Frame f;
    f.type = wire::Type::Snapshot;
    f.from = link_.self();
    f.snapshot.phase = static_cast<uint8_t>(view_.phase);
    f.snapshot.host = view_.host;
    f.snapshot.winner = view_.winner;
    f.snapshot.candidates = view_.candidates;
    link_.send(to, wire::encode(f), true);
}

PlayerId SessionNet::lowest_connected() const {
    for (PlayerId p = kFirstPlayer; p <= kMaxPlayers; ++p) {
        if (connected_[p] || p == link_.self()) return p;
    }
    return link_.self();
}

} // namespace net
