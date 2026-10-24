#pragma once
// Multiplayer session over the mesh: connects the session rules (session.h) to the network (enet_link.h).
//
// Authority: one player at a time runs the HostSession and sends its notices to everyone. That is the
// lowest connected player id, and it stays with the current authority until that player leaves. When
// the authority leaves, the lowest id left takes over: it starts a session for the game as it stands
// (pause, then the takeover vote after the usual delay), and the others follow its notices.
//
// Other players do not run the rules. They apply the authority's notices to their view, send their
// votes and acknowledgements to the authority, and receive a Snapshot when they join or come back.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "net/enet_link.h"
#include "net/session.h"

namespace net {

// What this player currently sees of the session.
struct SessionView {
    Phase phase = Phase::Running;
    PlayerId host = kFirstPlayer;
    PlayerId winner = 0;              // the drawn player while AwaitingAck, else 0
    std::vector<PlayerId> candidates; // eligible voters, or the draw's candidates
};

class SessionNet {
public:
    SessionNet(EnetLink& link, HostSession::Draw draw);

    // Call once the link is open and the mesh is formed. If this player is the authority, the
    // session starts here.
    void start(uint64_t now_ms);

    // Call after EnetLink::service(). Handles connections, messages and the session's timers.
    void tick(uint64_t now_ms);

    // This player's vote on a takeover, sent to the authority (or applied, if this is the authority).
    void vote(bool wants_host, uint64_t now_ms);

    const SessionView& view() const { return view_; }
    PlayerId authority() const { return authority_; }
    bool is_authority() const { return authority_ == link_.self(); }

    // Notices applied to the view since the last call, for the game's messages.
    std::vector<Notice> take_events();

private:
    void on_connected(PlayerId p, uint64_t now_ms);
    void on_disconnected(PlayerId p, uint64_t now_ms);
    void on_frame(const Inbound& in, uint64_t now_ms);
    void adopt(uint64_t now_ms);
    void release();
    void publish(std::vector<Notice> notices, uint64_t now_ms);
    void apply(const Notice& n);
    void acknowledge(uint64_t now_ms);
    void send_snapshot(PlayerId to);
    PlayerId lowest_connected() const;

    EnetLink& link_;
    HostSession::Draw draw_;
    std::unique_ptr<HostSession> session_; // only on the authority
    PlayerId authority_ = kFirstPlayer;
    SessionView view_;
    std::vector<Notice> events_;
    std::array<bool, kMaxPlayers + 1> connected_{};
};

} // namespace net
