"""HTTP front end for the lobby registry (lobby.py). Standard library only.

Routes (all bodies and answers are JSON):
  GET  /lobbies                          list open games
  POST /lobbies                          {"name", "port"}          host a game
  POST /lobbies/<id>/join                {"port"}                  join a game
  POST /lobbies/<id>/heartbeat           {"token"}                 keep your place
  POST /lobbies/<id>/leave               {"token"}                 leave

The address of each member is the one the server sees on the request, so a client cannot give
someone else's. Only the game port is sent by the client.

Errors: {"error": "<code>", "message": "<text>"} with the matching HTTP status.
"""

from __future__ import annotations

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

from lobby import LobbyError, Registry

MAX_BODY = 4096


def _handler_for(registry: Registry, lock: threading.Lock):
    class Handler(BaseHTTPRequestHandler):
        server_version = "WWHDLobby/1"

        def log_message(self, fmt, *args):  # keep the console quiet
            pass

        def do_GET(self):
            if self.path != "/lobbies":
                return self._send(404, {"error": "not_found", "message": "no such route"})
            with lock:
                self._send(200, {"games": registry.list()})

        def do_POST(self):
            try:
                body = self._read_body()
                ip = self.client_address[0]
                with lock:
                    answer = self._route(body, ip)
                self._send(200, answer)
            except LobbyError as e:
                self._send(e.status, {"error": e.code, "message": e.message})
            except ValueError as e:
                self._send(400, {"error": "bad_request", "message": str(e)})

        def _route(self, body: dict, ip: str) -> dict:
            parts = self.path.strip("/").split("/")
            if parts == ["lobbies"]:
                return registry.create(body.get("name"), ip, body.get("port"))
            if len(parts) == 3 and parts[0] == "lobbies":
                lobby_id, action = parts[1], parts[2]
                if action == "join":
                    return registry.join(lobby_id, ip, body.get("port"))
                if action == "heartbeat":
                    registry.heartbeat(lobby_id, body.get("token"))
                    return {}
                if action == "leave":
                    registry.leave(lobby_id, body.get("token"))
                    return {}
            raise LobbyError(404, "not_found", "no such route")

        def _read_body(self) -> dict:
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                raise ValueError("bad Content-Length")
            if length < 0 or length > MAX_BODY:
                raise ValueError("body too large")
            raw = self.rfile.read(length) if length else b""
            if not raw:
                return {}
            try:
                data = json.loads(raw)
            except json.JSONDecodeError:
                raise ValueError("body is not valid JSON")
            if not isinstance(data, dict):
                raise ValueError("body must be a JSON object")
            return data

        def _send(self, status: int, payload: dict) -> None:
            raw = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

    return Handler


def make_server(host: str = "127.0.0.1", port: int = 0, registry: Optional[Registry] = None) -> ThreadingHTTPServer:
    registry = registry or Registry()
    lock = threading.Lock()
    return ThreadingHTTPServer((host, port), _handler_for(registry, lock))


def main() -> None:
    parser = argparse.ArgumentParser(description="WWHD multiplayer lobby server")
    parser.add_argument("--host", default="0.0.0.0", help="address to listen on (default: all)")
    parser.add_argument("--port", type=int, default=8787, help="HTTP port (default: 8787)")
    args = parser.parse_args()
    server = make_server(args.host, args.port)
    print(f"lobby listening on {args.host}:{server.server_address[1]}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
