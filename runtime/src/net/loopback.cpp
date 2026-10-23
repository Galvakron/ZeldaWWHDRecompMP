#include "net/loopback.h"

namespace net {

namespace {
bool valid(PlayerId p) {
    return p >= kFirstPlayer && p <= kMaxPlayers;
}
} // namespace

void LoopbackHub::join(PlayerId p) {
    if (!valid(p)) return;
    slots_[p].joined = true;
    slots_[p].inbox.clear();
}

void LoopbackHub::leave(PlayerId p) {
    if (!valid(p)) return;
    slots_[p].joined = false;
    slots_[p].inbox.clear();
}

bool LoopbackHub::joined(PlayerId p) const {
    return valid(p) && slots_[p].joined;
}

void LoopbackHub::send(PlayerId from, PlayerId to, std::vector<uint8_t> bytes) {
    if (!joined(from) || !joined(to)) return;
    slots_[to].inbox.emplace_back(from, std::move(bytes));
}

bool LoopbackHub::receive(PlayerId p, PlayerId& from, std::vector<uint8_t>& bytes) {
    if (!joined(p) || slots_[p].inbox.empty()) return false;
    from = slots_[p].inbox.front().first;
    bytes = std::move(slots_[p].inbox.front().second);
    slots_[p].inbox.pop_front();
    return true;
}

} // namespace net
