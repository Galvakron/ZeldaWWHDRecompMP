"""Tests for the lobby registry and its HTTP front end. Run: python3 -m unittest -v test_lobby"""

import json
import threading
import unittest
import urllib.error
import urllib.request

from lobby import HEARTBEAT_TTL, MAX_PLAYERS, LobbyError, Registry
from server import make_server


class FakeClock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.reg = Registry(clock=self.clock)

    def test_create_makes_host_player_one(self):
        game = self.reg.create("Lien vert", "203.0.113.5", 47000)
        self.assertEqual(game["player"], 1)
        self.assertEqual(game["roster"], [])
        listed = self.reg.list()
        self.assertEqual(len(listed), 1)
        self.assertEqual(listed[0]["name"], "Lien vert")
        self.assertEqual(listed[0]["players"], 1)

    def test_join_returns_roster_of_existing_members(self):
        game = self.reg.create("Partie", "203.0.113.5", 47000)
        joined = self.reg.join(game["id"], "198.51.100.7", 47001)
        self.assertEqual(joined["player"], 2)
        self.assertEqual(joined["roster"], [{"player": 1, "ip": "203.0.113.5", "port": 47000}])

    def test_full_game_refuses_a_fifth_player(self):
        game = self.reg.create("Pleine", "203.0.113.5", 47000)
        for port in range(47001, 47001 + MAX_PLAYERS - 1):
            self.reg.join(game["id"], "198.51.100.7", port)
        with self.assertRaises(LobbyError) as ctx:
            self.reg.join(game["id"], "198.51.100.8", 47100)
        self.assertEqual(ctx.exception.status, 409)
        self.assertEqual(ctx.exception.code, "full")

    def test_leaving_frees_the_player_id(self):
        game = self.reg.create("Ids", "203.0.113.5", 47000)
        second = self.reg.join(game["id"], "198.51.100.7", 47001)
        third = self.reg.join(game["id"], "198.51.100.8", 47002)
        self.assertEqual((second["player"], third["player"]), (2, 3))
        self.reg.leave(game["id"], second["token"])
        fourth = self.reg.join(game["id"], "198.51.100.9", 47003)
        self.assertEqual(fourth["player"], 2)

    def test_empty_lobby_disappears(self):
        game = self.reg.create("Seul", "203.0.113.5", 47000)
        self.reg.leave(game["id"], game["token"])
        self.assertEqual(self.reg.list(), [])
        with self.assertRaises(LobbyError) as ctx:
            self.reg.join(game["id"], "198.51.100.7", 47001)
        self.assertEqual(ctx.exception.status, 404)

    def test_silent_members_expire(self):
        game = self.reg.create("Silencieuse", "203.0.113.5", 47000)
        other = self.reg.join(game["id"], "198.51.100.7", 47001)
        self.clock.now += HEARTBEAT_TTL - 1
        self.reg.heartbeat(game["id"], other["token"])
        self.clock.now += HEARTBEAT_TTL - 1
        # The host has been silent for longer than the TTL, the other player has not.
        self.assertEqual(self.reg.list()[0]["players"], 1)
        self.clock.now += HEARTBEAT_TTL + 1
        self.assertEqual(self.reg.list(), [])

    def test_wrong_token_is_refused(self):
        game = self.reg.create("Securite", "203.0.113.5", 47000)
        with self.assertRaises(LobbyError) as ctx:
            self.reg.heartbeat(game["id"], "not-the-token")
        self.assertEqual(ctx.exception.status, 403)
        with self.assertRaises(LobbyError):
            self.reg.leave(game["id"], None)

    def test_bad_input_is_refused(self):
        bad_names = [None, "", "   ", "x" * 33, "tab\there"]
        for name in bad_names:
            with self.subTest(name=name):
                with self.assertRaises(LobbyError) as ctx:
                    self.reg.create(name, "203.0.113.5", 47000)
                self.assertEqual(ctx.exception.code, "bad_name")
        for port in [0, 65536, True, "47000", None]:
            with self.subTest(port=port):
                with self.assertRaises(LobbyError) as ctx:
                    self.reg.create("Ok", "203.0.113.5", port)
                self.assertEqual(ctx.exception.code, "bad_port")
        self.assertEqual(self.reg.list(), [])

    def test_unknown_game_is_not_found(self):
        with self.assertRaises(LobbyError) as ctx:
            self.reg.join("NOPE00", "198.51.100.7", 47001)
        self.assertEqual(ctx.exception.status, 404)


class HttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = make_server("127.0.0.1", 0)
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def call(self, method, path, body=None, raw=None):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req) as resp:
                return resp.status, json.loads(resp.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def test_host_list_join_leave(self):
        status, game = self.call("POST", "/lobbies", {"name": "Web", "port": 47010})
        self.assertEqual(status, 200)
        self.assertEqual(game["player"], 1)

        status, listed = self.call("GET", "/lobbies")
        self.assertEqual(status, 200)
        self.assertIn(game["id"], [g["id"] for g in listed["games"]])

        status, joined = self.call("POST", f"/lobbies/{game['id']}/join", {"port": 47011})
        self.assertEqual(status, 200)
        self.assertEqual(joined["player"], 2)
        self.assertEqual(joined["roster"][0]["port"], 47010)
        # The address comes from the request, so a client cannot claim another one.
        self.assertEqual(joined["roster"][0]["ip"], "127.0.0.1")

        status, _ = self.call("POST", f"/lobbies/{game['id']}/heartbeat", {"token": joined["token"]})
        self.assertEqual(status, 200)
        status, _ = self.call("POST", f"/lobbies/{game['id']}/leave", {"token": joined["token"]})
        self.assertEqual(status, 200)

    def test_errors_use_http_status_and_codes(self):
        status, body = self.call("POST", "/lobbies/NOPE00/join", {"port": 47012})
        self.assertEqual((status, body["error"]), (404, "not_found"))

        status, body = self.call("POST", "/lobbies", {"name": "", "port": 47012})
        self.assertEqual((status, body["error"]), (400, "bad_name"))

        status, body = self.call("POST", "/lobbies", raw=b"{not json")
        self.assertEqual((status, body["error"]), (400, "bad_request"))

        status, body = self.call("POST", "/lobbies", raw=b"[1, 2]")
        self.assertEqual((status, body["error"]), (400, "bad_request"))

        status, body = self.call("GET", "/nope")
        self.assertEqual(status, 404)

    def test_oversized_body_is_refused(self):
        status, body = self.call("POST", "/lobbies", raw=b"{" + b" " * 5000 + b"}")
        self.assertEqual((status, body["error"]), (400, "bad_request"))


if __name__ == "__main__":
    unittest.main()
