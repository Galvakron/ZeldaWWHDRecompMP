#include "net/wire.h"

#include <cstddef>

namespace net::wire {
namespace {

bool valid_player(uint8_t p) {
    return p >= kFirstPlayer && p <= kMaxPlayers;
}

void put_u8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr size_t kPlayerStateSize = 18;

void put_state(std::vector<uint8_t>& out, const PlayerState& s) {
    put_u32(out, static_cast<uint32_t>(s.x));
    put_u32(out, static_cast<uint32_t>(s.y));
    put_u32(out, static_cast<uint32_t>(s.z));
    put_u16(out, static_cast<uint16_t>(s.yaw));
    put_u16(out, s.anim);
    put_u8(out, s.scene);
    put_u8(out, s.room);
}

PlayerState get_state(const uint8_t* p) {
    PlayerState s;
    s.x = static_cast<int32_t>(get_u32(p));
    s.y = static_cast<int32_t>(get_u32(p + 4));
    s.z = static_cast<int32_t>(get_u32(p + 8));
    s.yaw = static_cast<int16_t>(get_u16(p + 12));
    s.anim = get_u16(p + 14);
    s.scene = p[16];
    s.room = p[17];
    return s;
}

std::vector<uint8_t> payload_of(const Frame& f) {
    std::vector<uint8_t> out;
    switch (f.type) {
    case Type::Hello:
    case Type::Disconnect:
    case Type::HostAck:
        break;
    case Type::Vote:
        put_u8(out, f.wants_host ? 1 : 0);
        break;
    case Type::Notice:
        put_u8(out, static_cast<uint8_t>(f.notice.kind));
        put_u8(out, f.notice.player);
        put_u8(out, static_cast<uint8_t>(f.notice.candidates.size()));
        for (PlayerId c : f.notice.candidates) put_u8(out, c);
        break;
    case Type::PlayerState:
        put_state(out, f.state);
        break;
    }
    return out;
}

// Checks the payload of one message type and fills the frame. False when it does not match the spec.
bool parse_payload(Type type, const uint8_t* p, size_t n, Frame& f) {
    switch (type) {
    case Type::Hello:
    case Type::Disconnect:
    case Type::HostAck:
        return n == 0;
    case Type::Vote:
        if (n != 1 || (p[0] != 0 && p[0] != 1)) return false;
        f.wants_host = p[0] == 1;
        return true;
    case Type::Notice: {
        if (n < 3) return false;
        const uint8_t kind = p[0];
        if (kind > static_cast<uint8_t>(NoticeKind::VoteFailed)) return false;
        const uint8_t count = p[2];
        if (count > kMaxPlayers || n != 3u + count) return false;
        if (!valid_player(p[1])) return false;
        f.notice.kind = static_cast<NoticeKind>(kind);
        f.notice.player = p[1];
        f.notice.candidates.clear();
        for (uint8_t i = 0; i < count; ++i) {
            if (!valid_player(p[3 + i])) return false;
            f.notice.candidates.push_back(p[3 + i]);
        }
        return true;
    }
    case Type::PlayerState:
        if (n != kPlayerStateSize) return false;
        f.state = get_state(p);
        return true;
    }
    return false;
}

} // namespace

std::vector<uint8_t> encode(const Frame& frame) {
    const std::vector<uint8_t> payload = payload_of(frame);
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + payload.size());
    put_u8(out, kVersion);
    put_u8(out, static_cast<uint8_t>(frame.type));
    put_u8(out, frame.from);
    put_u16(out, static_cast<uint16_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<Frame> decode(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < kHeaderSize) return std::nullopt;
    const uint8_t* d = bytes.data();
    if (d[0] != kVersion) return std::nullopt;
    if (d[1] < 1 || d[1] > static_cast<uint8_t>(Type::PlayerState)) return std::nullopt;
    if (!valid_player(d[2])) return std::nullopt;
    const size_t length = get_u16(d + 3);
    if (bytes.size() != kHeaderSize + length) return std::nullopt;

    Frame f;
    f.type = static_cast<Type>(d[1]);
    f.from = d[2];
    if (!parse_payload(f.type, d + kHeaderSize, length, f)) return std::nullopt;
    return f;
}

Frame frame_from_notice(const net::Notice& notice, PlayerId from) {
    Frame f;
    f.type = Type::Notice;
    f.from = from;
    f.notice = notice;
    return f;
}

} // namespace net::wire
