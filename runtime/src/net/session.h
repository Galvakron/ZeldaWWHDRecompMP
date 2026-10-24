#pragma once
// Multiplayer host session: what happens when the host leaves.
//
// The game pauses while the host is away. After kPauseBeforeTakeoverMs a 30 second vote opens among
// the other connected players. Those who vote "yes" are candidates, and the server draws one of them at
// random. The drawn player becomes host once they confirm they hold the game state (ack_host). If the
// original host returns before that, the vote or the draw is cancelled and the host is restored.
//
// This file is pure logic: no sockets, no game code, no clock. The caller passes the time in milliseconds
// and sends out the Notices it gets back.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace net {

using PlayerId = uint8_t;

constexpr PlayerId kFirstPlayer = 1; // the player who starts the session is the host
constexpr PlayerId kMaxPlayers = 4;

constexpr uint64_t kPauseBeforeTakeoverMs = 5ull * 60 * 1000;
constexpr uint64_t kTakeoverVoteMs = 30ull * 1000;
// Proposal, not in the spec: how long the drawn player has to confirm before the draw is redone.
constexpr uint64_t kHostAckTimeoutMs = 15ull * 1000;

enum class Phase {
    Running,      // a host is connected and the game runs
    Paused,       // the host is away; the game is frozen
    TakeoverVote, // 30 second vote open
    AwaitingAck,  // a candidate was drawn; waiting for them to confirm
};

enum class NoticeKind {
    PauseStarted,   // the host left: the game is paused
    TakeoverOpened, // the vote opened; candidates = players allowed to vote
    VoteDrawn,      // player = the drawn candidate; candidates = everyone who was in the draw
    HostChanged,    // player is now the host
    HostRestored,   // the original host came back and is the host again
    VoteFailed,     // nobody volunteered (or no candidate remained); the game stays paused
};

struct Notice {
    NoticeKind kind;
    PlayerId player = 0;
    std::vector<PlayerId> candidates;
};

class HostSession {
public:
    // Returns an index in [0, count). The server supplies a uniform random source. It is only called
    // when there are at least two candidates.
    using Draw = std::function<size_t(size_t count)>;

    // `host` is the player the game is running for: the one who left, when a new authority takes over.
    explicit HostSession(Draw draw, PlayerId host = kFirstPlayer);

    // Connection events. Unknown player IDs are ignored.
    void connect(PlayerId p, uint64_t now_ms);
    void disconnect(PlayerId p, uint64_t now_ms);

    // A vote during TakeoverVote. Only players who were connected when the vote opened can vote.
    // Voting again replaces the earlier answer. Votes in other phases are ignored.
    void vote(PlayerId p, bool wants_host, uint64_t now_ms);

    // The drawn candidate confirms they hold the state. Ignored unless they are the drawn candidate.
    void ack_host(PlayerId p, uint64_t now_ms);

    // Advances the timers. Call it regularly, e.g. once per frame or once per second.
    void tick(uint64_t now_ms);

    // Returns the notices produced since the last call and clears them.
    std::vector<Notice> take_notices();

    Phase phase() const { return phase_; }
    PlayerId host() const { return host_; }
    bool connected(PlayerId p) const;
    bool paused() const { return phase_ != Phase::Running; }

private:
    using Flags = std::array<bool, kMaxPlayers + 1>; // indexed by PlayerId, slot 0 unused

    bool valid(PlayerId p) const { return p >= kFirstPlayer && p <= kMaxPlayers; }
    void pause(uint64_t now_ms);
    void fail_vote(uint64_t now_ms);
    void open_vote(uint64_t now_ms);
    void resolve_vote(uint64_t now_ms);
    void draw_winner(uint64_t now_ms, const std::vector<PlayerId>& candidates);
    void restore_host(uint64_t now_ms);
    void clear_vote();
    std::vector<PlayerId> live_volunteers() const;

    Draw draw_;
    Phase phase_ = Phase::Running;
    PlayerId host_ = kFirstPlayer;
    Flags connected_{};
    Flags eligible_{};   // connected when the vote opened, excluding the host
    Flags volunteer_{};  // answered "yes" in this vote
    PlayerId winner_ = 0;
    uint64_t pause_since_ms_ = 0;
    uint64_t deadline_ms_ = 0; // vote end, or ack timeout in AwaitingAck
    std::vector<Notice> notices_;
};

} // namespace net
