#pragma once
// Wire format for the multiplayer messages: one frame = a fixed 5-byte header + a small payload.
//
//   byte 0      version (kVersion)
//   byte 1      message type (wire::Type)
//   byte 2      sender player id (1..4)
//   bytes 3-4   payload length, little endian
//   then        payload
//
// Decoding never trusts the input: wrong version, unknown type, bad length, out-of-range ids or
// values make decode() return nothing. Nothing here touches sockets or the game.

#include <cstdint>
#include <optional>
#include <vector>

#include "net/session.h"

namespace net::wire {

constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 5;

enum class Type : uint8_t {
    Hello = 1,       // a player joins the session (empty payload)
    Disconnect = 2,  // a player leaves (empty payload)
    Vote = 3,        // takeover vote: 1 byte, 0 or 1
    HostAck = 4,     // the drawn player confirms they hold the state (empty payload)
    Notice = 5,      // a session notice: kind, player, count, candidates
    PlayerState = 6, // a player's position and animation (18 bytes)
    Welcome = 7,     // the host gives a joining client its player id: 1 byte, 2..4
};

// State of a remote Link, sent a few times per second. Units are the game's own.
struct PlayerState {
    int32_t x = 0, y = 0, z = 0;
    int16_t yaw = 0;
    uint16_t anim = 0;
    uint8_t scene = 0;
    uint8_t room = 0;

    bool same_as(const PlayerState& o) const {
        return x == o.x && y == o.y && z == o.z && yaw == o.yaw && anim == o.anim && scene == o.scene &&
               room == o.room;
    }
};

struct Frame {
    Type type = Type::Hello;
    PlayerId from = 0;
    bool wants_host = false;  // Type::Vote
    net::Notice notice;       // Type::Notice
    PlayerState state;        // Type::PlayerState
    PlayerId assigned = 0;    // Type::Welcome
};

std::vector<uint8_t> encode(const Frame& frame);
std::optional<Frame> decode(const std::vector<uint8_t>& bytes);

// Notices map one to one to frames, so the host can relay session events as they happen.
Frame frame_from_notice(const net::Notice& notice, PlayerId from);

} // namespace net::wire
