#include "net/enet_link.h"

#include <cstdint>
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

bool valid_player(PlayerId p) {
    return p >= kFirstPlayer && p <= kMaxPlayers;
}

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

bool EnetLink::open(PlayerId self, uint16_t listen_port) {
    if (!enet_ready_ || host_ || !valid_player(self)) return false;

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = listen_port;
    // Room for every other player at once.
    host_ = enet_host_create(&address, kMaxPlayers, 1, 0, 0);
    if (!host_) return false;
    self_ = self;
    return true;
}

bool EnetLink::connect_to(const PeerAddress& peer) {
    if (!host_ || !valid_player(peer.player) || peer.player == self_) return false;
    if (peers_[peer.player]) return false; // already connected or connecting

    ENetAddress target{};
    if (enet_address_set_host(&target, peer.ip.c_str()) != 0) return false;
    target.port = peer.port;

    ENetPeer* connection = enet_host_connect(host_, &target, 1, 0);
    if (!connection) return false;
    connection->data = slot_data(peer.player);
    peers_[peer.player] = connection;
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
    if (!host_ || !joined(to)) return;
    ENetPacket* packet = make_packet(bytes, reliable);
    if (!packet) return;
    if (enet_peer_send(peers_[to], 0, packet) != 0) {
        enet_packet_destroy(packet); // ENet keeps the packet only on success
        return;
    }
    enet_host_flush(host_);
}

void EnetLink::broadcast(const std::vector<uint8_t>& bytes, bool reliable) {
    for (PlayerId p = kFirstPlayer; p <= kMaxPlayers; ++p) {
        if (p != self_) send(p, bytes, reliable);
    }
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
    return valid_player(p) && p != self_ && bound_[p];
}

void EnetLink::shutdown() {
    if (!host_) return;
    // Say goodbye to everyone, give ENet a moment to send it, then tear down.
    for (PlayerId p = kFirstPlayer; p <= kMaxPlayers; ++p) {
        if (peers_[p]) enet_peer_disconnect(peers_[p], 0);
    }
    enet_host_flush(host_);
    ENetEvent event{};
    for (int i = 0; i < 20 && enet_host_service(host_, &event, 10) >= 0; ++i) {
        if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
    }
    enet_host_destroy(host_);
    host_ = nullptr;
    peers_.fill(nullptr);
    bound_.fill(false);
    self_ = 0;
    inbox_.clear();
    events_.clear();
}

void EnetLink::on_connect(_ENetPeer* peer) {
    // Either side of a new connection introduces itself. The other side binds the connection to our id.
    send_hello(peer);
}

void EnetLink::on_receive(_ENetPeer* peer, const uint8_t* data, size_t size) {
    std::vector<uint8_t> bytes(data, data + size);
    auto frame = wire::decode(bytes);

    if (frame && frame->type == wire::Type::Hello) {
        bind(peer, frame->from);
        return;
    }

    // Anything else is only accepted from a bound connection; its sender is the bound player.
    const PlayerId from = slot_of(peer);
    if (!valid_player(from) || !bound_[from] || peers_[from] != peer) return;
    inbox_.push_back({from, std::move(bytes)});
}

void EnetLink::on_disconnect(_ENetPeer* peer) {
    const PlayerId slot = slot_of(peer);
    peer->data = nullptr;
    if (!valid_player(slot) || peers_[slot] != peer) return;
    peers_[slot] = nullptr;
    if (bound_[slot]) {
        bound_[slot] = false;
        events_.push_back({LinkEventKind::Disconnected, slot});
    }
}

void EnetLink::bind(_ENetPeer* peer, PlayerId player) {
    if (!valid_player(player) || player == self_) {
        enet_peer_disconnect(peer, 0);
        return;
    }
    if (peers_[player] && peers_[player] != peer) {
        // A second connection to the same player: keep the one already there.
        enet_peer_disconnect(peer, 0);
        return;
    }
    peer->data = slot_data(player);
    peers_[player] = peer;
    if (!bound_[player]) {
        bound_[player] = true;
        events_.push_back({LinkEventKind::Connected, player});
    }
}

void EnetLink::send_hello(_ENetPeer* peer) {
    wire::Frame hello;
    hello.type = wire::Type::Hello;
    hello.from = self_;
    ENetPacket* packet = make_packet(wire::encode(hello), true);
    if (packet && enet_peer_send(peer, 0, packet) != 0) enet_packet_destroy(packet);
    enet_host_flush(host_);
}

} // namespace net
