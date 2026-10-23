#include "net/enet_link.h"

#include <cstdint>
#include <cstring>
#include <utility>

#include <enet/enet.h>

#include "net/wire.h"

namespace net {
namespace {

int g_enet_users = 0;

void* slot_data(PlayerId p) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(p));
}

PlayerId slot_of(const _ENetPeer* peer) {
    return static_cast<PlayerId>(reinterpret_cast<uintptr_t>(peer->data));
}

// Builds a packet from bytes. Caller passes it to enet_peer_send, which keeps it only on success.
ENetPacket* make_packet(const std::vector<uint8_t>& bytes, bool reliable) {
    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    return enet_packet_create(bytes.data(), bytes.size(), flags);
}

} // namespace

EnetLink::EnetLink() {
    if (g_enet_users == 0 && enet_initialize() != 0) return;
    ++g_enet_users;
    enet_ready_ = true;
}

EnetLink::~EnetLink() {
    shutdown();
    if (enet_ready_ && --g_enet_users == 0) enet_deinitialize();
}

bool EnetLink::host(uint16_t port, int max_clients) {
    if (!enet_ready_ || mode_ != Mode::None) return false;
    if (max_clients < 1) max_clients = 1;
    if (max_clients > kMaxPlayers - 1) max_clients = kMaxPlayers - 1;

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    host_ = enet_host_create(&address, static_cast<size_t>(max_clients), 1, 0, 0);
    if (!host_) return false;
    mode_ = Mode::Host;
    self_ = kFirstPlayer;
    return true;
}

bool EnetLink::connect(const char* address, uint16_t port) {
    if (!enet_ready_ || mode_ != Mode::None || !address) return false;

    ENetAddress target{};
    if (enet_address_set_host(&target, address) != 0) return false;
    target.port = port;

    host_ = enet_host_create(nullptr, 1, 1, 0, 0);
    if (!host_) return false;
    server_peer_ = enet_host_connect(host_, &target, 1, 0);
    if (!server_peer_) {
        enet_host_destroy(host_);
        host_ = nullptr;
        return false;
    }
    mode_ = Mode::Client;
    self_ = 0;
    return true;
}

void EnetLink::service(uint32_t timeout_ms) {
    if (!host_) return;
    ENetEvent event{};
    int result = enet_host_service(host_, &event, timeout_ms);
    while (result > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            on_connect(event.peer);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            on_receive(event.peer, event.packet->data, event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            on_disconnect(event.peer);
            break;
        default:
            break;
        }
        result = enet_host_service(host_, &event, 0);
    }
}

void EnetLink::send(PlayerId to, const std::vector<uint8_t>& bytes, bool reliable) {
    _ENetPeer* peer = peer_for(to);
    if (!peer || !host_) return;
    ENetPacket* packet = make_packet(bytes, reliable);
    if (!packet) return;
    if (enet_peer_send(peer, 0, packet) != 0) {
        enet_packet_destroy(packet); // not taken over on failure
        return;
    }
    enet_host_flush(host_);
}

void EnetLink::broadcast(const std::vector<uint8_t>& bytes, bool reliable) {
    if (mode_ != Mode::Host) return;
    for (PlayerId p = kFirstPlayer + 1; p <= kMaxPlayers; ++p) send(p, bytes, reliable);
}

bool EnetLink::poll(Inbound& out) {
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.erase(inbox_.begin());
    return true;
}

bool EnetLink::poll_event(LinkEvent& out) {
    if (events_.empty()) return false;
    out = events_.front();
    events_.erase(events_.begin());
    return true;
}

bool EnetLink::joined(PlayerId p) const {
    if (mode_ == Mode::Host) {
        if (p == kFirstPlayer) return true;
        return p > kFirstPlayer && p <= kMaxPlayers && peers_[p] != nullptr;
    }
    if (mode_ == Mode::Client) {
        if (p == kFirstPlayer) return server_peer_ != nullptr;
        return self_ != 0 && p == self_ && server_peer_ != nullptr;
    }
    return false;
}

void EnetLink::shutdown() {
    if (!host_) {
        mode_ = Mode::None;
        return;
    }
    // Say goodbye to every connected peer, give ENet a moment to send it, then tear down.
    if (mode_ == Mode::Host) {
        for (PlayerId p = kFirstPlayer + 1; p <= kMaxPlayers; ++p) {
            if (peers_[p]) enet_peer_disconnect(peers_[p], 0);
        }
    } else if (server_peer_) {
        enet_peer_disconnect(server_peer_, 0);
    }
    enet_host_flush(host_);
    ENetEvent event{};
    for (int i = 0; i < 20 && enet_host_service(host_, &event, 10) >= 0; ++i) {
        if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
    }
    enet_host_destroy(host_);
    host_ = nullptr;
    peers_.fill(nullptr);
    server_peer_ = nullptr;
    mode_ = Mode::None;
    self_ = 0;
    inbox_.clear();
    events_.clear();
}

void EnetLink::on_connect(_ENetPeer* peer) {
    if (mode_ != Mode::Host) return; // a client waits for Welcome from the host

    PlayerId slot = 0;
    for (PlayerId p = kFirstPlayer + 1; p <= kMaxPlayers; ++p) {
        if (!peers_[p]) {
            slot = p;
            break;
        }
    }
    if (slot == 0) {
        peer->data = nullptr;
        enet_peer_disconnect(peer, 0); // full: no slot left
        return;
    }

    peers_[slot] = peer;
    peer->data = slot_data(slot);

    wire::Frame welcome;
    welcome.type = wire::Type::Welcome;
    welcome.from = kFirstPlayer;
    welcome.assigned = slot;
    ENetPacket* packet = make_packet(wire::encode(welcome), true);
    if (packet && enet_peer_send(peer, 0, packet) != 0) enet_packet_destroy(packet);

    events_.push_back({LinkEventKind::Connected, slot});
}

void EnetLink::on_receive(_ENetPeer* peer, const uint8_t* data, size_t size) {
    std::vector<uint8_t> bytes(data, data + size);

    if (mode_ == Mode::Client) {
        auto frame = wire::decode(bytes);
        if (frame && frame->type == wire::Type::Welcome) {
            if (self_ == 0) {
                self_ = frame->assigned;
                events_.push_back({LinkEventKind::Connected, self_});
            }
            return; // handled here, not passed on
        }
        inbox_.push_back({kFirstPlayer, std::move(bytes)});
        return;
    }

    if (mode_ == Mode::Host) {
        const PlayerId from = slot_of(peer);
        if (from == 0) return; // a peer without a slot (refused): ignore
        inbox_.push_back({from, std::move(bytes)});
    }
}

void EnetLink::on_disconnect(_ENetPeer* peer) {
    if (mode_ == Mode::Host) {
        const PlayerId slot = slot_of(peer);
        peer->data = nullptr;
        if (slot > kFirstPlayer && slot <= kMaxPlayers && peers_[slot] == peer) {
            peers_[slot] = nullptr;
            events_.push_back({LinkEventKind::Disconnected, slot});
        }
        return;
    }
    if (mode_ == Mode::Client && peer == server_peer_) {
        server_peer_ = nullptr;
        events_.push_back({LinkEventKind::Disconnected, kFirstPlayer});
    }
}

_ENetPeer* EnetLink::peer_for(PlayerId p) const {
    if (mode_ == Mode::Host) {
        if (p <= kFirstPlayer || p > kMaxPlayers) return nullptr;
        return peers_[p];
    }
    if (mode_ == Mode::Client && p == kFirstPlayer) return server_peer_;
    return nullptr;
}

} // namespace net
