#pragma once
// In-memory transport for tests and for running the session logic without sockets.
// Messages go from one player's inbox to another's, in order, with no delay and no loss.
// A real transport (ENet or similar) will offer the same operations to the session code.

#include <array>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "net/session.h"

namespace net {

class LoopbackHub {
public:
    // A player joins: their inbox starts empty. Unknown IDs are ignored.
    void join(PlayerId p);

    // A player leaves: their queued messages are dropped, and nothing more is delivered to them.
    void leave(PlayerId p);

    bool joined(PlayerId p) const;

    // Queues bytes for `to`. Ignored unless both players have joined.
    void send(PlayerId from, PlayerId to, std::vector<uint8_t> bytes);

    // Takes the oldest message for `p`. Returns false when the inbox is empty.
    bool receive(PlayerId p, PlayerId& from, std::vector<uint8_t>& bytes);

private:
    struct Slot {
        bool joined = false;
        std::deque<std::pair<PlayerId, std::vector<uint8_t>>> inbox;
    };
    std::array<Slot, kMaxPlayers + 1> slots_{};
};

} // namespace net
