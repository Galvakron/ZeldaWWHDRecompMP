#pragma once
// Network transport on top of ENet (https://github.com/lsalzman/enet, v1.3.18).
//
// Topology: a star. The host is player 1 and runs the game. Clients connect to the host, which
// hands out player ids 2..4 in connection order (Welcome frame). Clients send only to the host,
// and the host relays what the session needs to the other clients.
//
// This layer moves bytes. It does not decode the game messages (see wire.h) and does not decide
// anything about the session (see session.h); its only job is to report connections and bytes.

#include <array>
#include <cstdint>
#include <vector>

#include "net/session.h"

struct _ENetHost;
struct _ENetPeer;

namespace net {

// Connections as seen by this process. For the host, `player` is the joining client's id. For a
// client, `player` is its own id (on Welcome) or the host (1) when the connection is lost.
enum class LinkEventKind { Connected, Disconnected };

struct LinkEvent {
    LinkEventKind kind;
    PlayerId player;
};

struct Inbound {
    PlayerId from = 0; // the sender as seen by this process (for a client: always 1, the host)
    std::vector<uint8_t> bytes;
};

class EnetLink {
public:
    EnetLink();
    ~EnetLink();
    EnetLink(const EnetLink&) = delete;
    EnetLink& operator=(const EnetLink&) = delete;

    // Starts a host listening on `port`, with room for `max_clients` (at most 3). Returns false on failure.
    bool host(uint16_t port, int max_clients = kMaxPlayers - 1);

    // Connects a client to a host at `address` (IPv4 or a host name) and `port`. Returns false on failure.
    bool connect(const char* address, uint16_t port);

    // Pumps the network for up to `timeout_ms`, handling connections, Welcome frames and disconnects.
    void service(uint32_t timeout_ms);

    // Sends to one player. The host can send to any joined client; a client can only send to the host (1).
    // Reliable messages are re-sent until acknowledged and arrive in order; unreliable ones may be lost.
    void send(PlayerId to, const std::vector<uint8_t>& bytes, bool reliable);

    // Host only: sends to every joined client.
    void broadcast(const std::vector<uint8_t>& bytes, bool reliable);

    // Takes the oldest received message. Returns false when there is none.
    bool poll(Inbound& out);

    // Takes the oldest connection event. Returns false when there is none.
    bool poll_event(LinkEvent& out);

    // This process's own player id: 1 for the host, the assigned id for a client, 0 until Welcome.
    PlayerId self() const { return self_; }
    bool is_host() const { return mode_ == Mode::Host; }
    bool joined(PlayerId p) const;

    void shutdown();

private:
    enum class Mode { None, Host, Client };

    void on_connect(_ENetPeer* peer);
    void on_receive(_ENetPeer* peer, const uint8_t* data, size_t size);
    void on_disconnect(_ENetPeer* peer);
    _ENetPeer* peer_for(PlayerId p) const;

    Mode mode_ = Mode::None;
    PlayerId self_ = 0;
    _ENetHost* host_ = nullptr;
    _ENetPeer* server_peer_ = nullptr;             // client mode: the connection to the host
    std::array<_ENetPeer*, kMaxPlayers + 1> peers_{}; // host mode: slot -> peer
    std::vector<Inbound> inbox_;
    std::vector<LinkEvent> events_;
    bool enet_ready_ = false;
};

} // namespace net
