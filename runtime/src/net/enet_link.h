#pragma once
// Network transport on top of ENet (https://github.com/lsalzman/enet, v1.3.18).
//
// Topology: a full mesh. Every player listens on one port and is connected to every other player.
// There is no central server, so when the host leaves, the others still reach each other and can
// run the takeover (see session.h).
//
// Connecting: a player who joins the lobby gets the roster (the players already there) and calls
// connect_to() for each of them. Existing players only accept. Each side sends a Hello frame when a
// connection opens, and the connection is bound to that player id when the Hello arrives.
//
// This layer moves bytes. It does not decode the game messages (see wire.h) and does not decide
// anything about the session (see session.h).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "net/session.h"

struct _ENetHost;
struct _ENetPeer;

namespace net {

// Connections as seen by this process: a player is connected once their Hello has been received
// (or, for a connection we opened, once it is up).
enum class LinkEventKind { Connected, Disconnected };

struct LinkEvent {
    LinkEventKind kind;
    PlayerId player;
};

struct Inbound {
    PlayerId from = 0;
    std::vector<uint8_t> bytes;
};

// Where another player can be reached. The address is the one given by the lobby.
struct PeerAddress {
    PlayerId player = 0;
    std::string ip;
    uint16_t port = 0;
};

class EnetLink {
public:
    EnetLink();
    ~EnetLink();
    EnetLink(const EnetLink&) = delete;
    EnetLink& operator=(const EnetLink&) = delete;

    // Starts listening on `listen_port` as player `self` (1..4). Returns false on failure.
    bool open(PlayerId self, uint16_t listen_port);

    // Opens a connection to another player. Returns false if the address is unusable.
    bool connect_to(const PeerAddress& peer);

    // Pumps the network for up to `timeout_ms`.
    void service(uint32_t timeout_ms);

    // Sends to one player, reliably or not. Reliable messages arrive in order and are re-sent until acknowledged.
    void send(PlayerId to, const std::vector<uint8_t>& bytes, bool reliable);

    // Sends to every connected player.
    void broadcast(const std::vector<uint8_t>& bytes, bool reliable);

    bool poll(Inbound& out);
    bool poll_event(LinkEvent& out);

    PlayerId self() const { return self_; }
    bool joined(PlayerId p) const;

    void shutdown();

private:
    void on_connect(_ENetPeer* peer);
    void on_receive(_ENetPeer* peer, const uint8_t* data, size_t size);
    void on_disconnect(_ENetPeer* peer);
    void bind(_ENetPeer* peer, PlayerId player);
    void send_hello(_ENetPeer* peer);

    PlayerId self_ = 0;
    _ENetHost* host_ = nullptr;
    std::array<_ENetPeer*, kMaxPlayers + 1> peers_{}; // player -> connection (bound or not yet)
    std::array<bool, kMaxPlayers + 1> bound_{};       // the connection's Hello has been received
    std::vector<Inbound> inbox_;
    std::vector<LinkEvent> events_;
    bool enet_ready_ = false;
};

} // namespace net
