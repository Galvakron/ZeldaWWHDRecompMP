"""Lobby registry for the multiplayer game: who hosts a game, and who has joined it.

This module is pure logic: no network code, and the clock is passed in, so it can be tested without
waiting. The HTTP layer is in server.py.

Rules:
- A game has at most MAX_PLAYERS members. Player ids 1..4 are handed out in join order, and an id
  freed by a leave is reused.
- Each member keeps the lobby alive with a heartbeat. A member who has been silent for longer than
  HEARTBEAT_TTL is removed. A lobby with no members left is removed.
- The address a member is reachable at is the one the server saw on the request (not what the client
  claims), plus the game port the client gives.
"""

from __future__ import annotations

import secrets
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

MAX_PLAYERS = 4
HEARTBEAT_TTL = 60.0
NAME_MAX = 32
# Without 0, O, 1, I and L, so that codes read out loud are not ambiguous.
CODE_ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
CODE_LENGTH = 6


class LobbyError(Exception):
    """An error with the HTTP status and the machine-readable code to return to the client."""

    def __init__(self, status: int, code: str, message: str):
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message


@dataclass
class Member:
    player: int
    ip: str
    port: int
    token: str
    last_seen: float


@dataclass
class Lobby:
    id: str
    name: str
    members: list[Member] = field(default_factory=list)


def check_name(value: object) -> str:
    if not isinstance(value, str):
        raise LobbyError(400, "bad_name", "the name must be a string")
    name = value.strip()
    if not 1 <= len(name) <= NAME_MAX or not name.isprintable():
        raise LobbyError(400, "bad_name", f"the name must be 1 to {NAME_MAX} printable characters")
    return name


def check_port(value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 65535:
        raise LobbyError(400, "bad_port", "the port must be a number from 1 to 65535")
    return value


class Registry:
    def __init__(self, clock: Callable[[], float] = time.monotonic):
        self._clock = clock
        self._lobbies: dict[str, Lobby] = {}

    # Public operations. Each one first drops members who stopped sending heartbeats.

    def create(self, name: object, ip: str, port: object) -> dict:
        self._expire()
        name = check_name(name)
        port = check_port(port)
        lobby_id = self._new_id()
        member = Member(1, ip, port, secrets.token_urlsafe(16), self._clock())
        self._lobbies[lobby_id] = Lobby(lobby_id, name, [member])
        return {"id": lobby_id, "token": member.token, "player": member.player, "roster": []}

    def list(self) -> list[dict]:
        self._expire()
        return [
            {"id": lobby.id, "name": lobby.name, "players": len(lobby.members), "max": MAX_PLAYERS}
            for lobby in self._lobbies.values()
        ]

    def join(self, lobby_id: str, ip: str, port: object) -> dict:
        self._expire()
        port = check_port(port)
        lobby = self._find(lobby_id)
        if len(lobby.members) >= MAX_PLAYERS:
            raise LobbyError(409, "full", "this game is full")
        # The joiner connects to everyone already there, so the roster is sent before adding them.
        roster = [{"player": m.player, "ip": m.ip, "port": m.port} for m in lobby.members]
        used = {m.player for m in lobby.members}
        player = next(p for p in range(1, MAX_PLAYERS + 1) if p not in used)
        member = Member(player, ip, port, secrets.token_urlsafe(16), self._clock())
        lobby.members.append(member)
        lobby.members.sort(key=lambda m: m.player)
        return {"id": lobby.id, "token": member.token, "player": player, "roster": roster}

    def heartbeat(self, lobby_id: str, token: object) -> None:
        self._expire()
        lobby = self._find(lobby_id)
        self._member(lobby, token).last_seen = self._clock()

    def leave(self, lobby_id: str, token: object) -> None:
        self._expire()
        lobby = self._find(lobby_id)
        member = self._member(lobby, token)
        lobby.members.remove(member)
        if not lobby.members:
            del self._lobbies[lobby.id]

    # Internal helpers.

    def _expire(self) -> None:
        now = self._clock()
        for lobby in list(self._lobbies.values()):
            lobby.members = [m for m in lobby.members if now - m.last_seen <= HEARTBEAT_TTL]
            if not lobby.members:
                del self._lobbies[lobby.id]

    def _find(self, lobby_id: object) -> Lobby:
        lobby: Optional[Lobby] = self._lobbies.get(lobby_id) if isinstance(lobby_id, str) else None
        if lobby is None:
            raise LobbyError(404, "not_found", "no such game")
        return lobby

    def _member(self, lobby: Lobby, token: object) -> Member:
        for member in lobby.members:
            if isinstance(token, str) and secrets.compare_digest(member.token, token):
                return member
        raise LobbyError(403, "bad_token", "not a member of this game")

    def _new_id(self) -> str:
        while True:
            lobby_id = "".join(secrets.choice(CODE_ALPHABET) for _ in range(CODE_LENGTH))
            if lobby_id not in self._lobbies:
                return lobby_id
