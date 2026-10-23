#!/usr/bin/env python3
"""Host-side ABI/API tests; no device, UI, network or playback required."""
import ctypes
import json
import pathlib
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        library = pathlib.Path(cls.temp.name) / "nowplaying.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-std=c99", "-Wall", "-Wextra",
                        "-I", str(ROOT / "native/sbbase/include"),
                        str(ROOT / "native/sbbase/src/nowplaying.c"), "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.np_create.argtypes = [ctypes.c_uint, ctypes.c_uint]
        cls.lib.np_create.restype = ctypes.c_void_p
        cls.lib.np_destroy.argtypes = [ctypes.c_void_p]
        cls.lib.np_public_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        cls.lib.np_expire.argtypes = [ctypes.c_void_p]
        cls.lib.np_api.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                                  ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_int)]
        cls.lib.np_lms_query_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                             ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.manager = self.lib.np_create(120, 8)
        _, claim, _ = self.api("claim", {"source": "test", "name": "Test"})
        self.token = claim["session_id"]

    def tearDown(self):
        self.lib.np_destroy(self.manager)

    def api(self, action, payload):
        body = json.dumps(payload, ensure_ascii=False).encode()
        output = ctypes.create_string_buffer(16384)
        changed = ctypes.c_int()
        code = self.lib.np_api(self.manager, b"POST", ("/api/nowplaying/" + action).encode(),
                               body, len(body), output, len(output), ctypes.byref(changed))
        return code, json.loads(output.value), changed.value

    def update(self, **values):
        return self.api("update", dict(session_id=self.token, **values))

    def query(self, *command, capacity=16384):
        body = json.dumps({"data": {"request": ["00:00:00:00:00:01", command]}}).encode()
        output = ctypes.create_string_buffer(capacity)
        self.assertEqual(self.lib.np_lms_query_json(self.manager, body, len(body),
                                                   output, len(output)), 1)
        return json.loads(output.value)

    def start(self, audio=None, **track):
        payload = {"id": "a", "title": "Song", "artist": "Artist"}
        payload.update(track)
        if audio is not None:
            payload["audio"] = audio
        self.assertEqual(self.update(state="playing", track=payload)[0], 200)

    def current(self, tags="roTIHQ"):
        return self.query("status", "-", 1, "tags:" + tags)["playlist_loop"][0]

    def test_legacy_and_unknown(self):
        self.start()
        self.assertEqual(self.current("a")["artist"], "Artist")
        self.assertNotIn("type", self.current())
        self.assertNotIn("bitrate", self.current())
        self.assertNotIn("lossless", self.current())

    def test_mp3_aac_and_vbr(self):
        for codec, rate, mode in [("mp3", 128, "CBR"), ("aac", 48, ""), ("mp3", 192, "VBR")]:
            self.start({"codec": codec, "bitrate_kbps": rate, "bitrate_mode": mode})
            self.assertEqual(self.current()["type"], codec)
            self.assertEqual(self.current()["bitrate"], str(rate) + "kbps" + (" " + mode if mode else ""))

    def test_lossless_source_and_transport(self):
        self.start({"codec": "flc", "sample_rate_hz": 96000, "bit_depth": 24,
                    "channels": 2, "lossless": True, "replaygain_db": -3.25},
                   transport={"codec": "pcm", "sample_rate_hz": 48000, "bit_depth": 16})
        status = self.query("status", "-", 1, "tags:oTIHQrY")
        source = status["playlist_loop"][0]
        self.assertEqual((source["type"], source["samplerate"], source["samplesize"]), ("flc", 96000, 24))
        self.assertEqual((source["channels"], source["lossless"], source["replay_gain"]), (2, 1, -3.25))
        self.assertNotIn("bitrate", source)
        self.assertEqual((status["type"], status["samplerate"], status["samplesize"]), ("pcm", 48000, 16))

    def test_tag_filtering_and_duration(self):
        self.start({"codec": "mp3", "bitrate_kbps": 128}, duration_ms=12500)
        self.assertEqual(set(self.current("r")), {"playlist index", "id", "title", "bitrate"})
        self.assertNotIn("bitrate", self.current("b"))
        self.assertEqual(self.current("d")["duration"], 12.5)
        self.update(live=True)
        self.assertNotIn("duration", self.current("d"))
        self.assertEqual(self.query("status", 1, 1)["playlist_loop"], [])

    def test_station_and_track_separation(self):
        self.start()
        self.update(live=True, station={"id": "station:a", "name": "Station", "description": "Not a comment",
                                       "homepage": "https://example.org", "genre": "Rock", "country": "NL"})
        track = self.current("aNgLk")
        self.assertEqual((track["title"], track["artist"], track["remote_title"]), ("Song", "Artist", "Station"))
        self.assertEqual((track["genre"], track["info_link"]), ("Rock", "https://example.org"))
        self.assertNotIn("comment", track)
        self.assertEqual(self.query("status", "-", 1, "menu:menu")["current_title"], "Station")
        output = ctypes.create_string_buffer(16384)
        self.assertGreater(self.lib.np_public_json(self.manager, output, len(output)), 0)
        self.assertEqual(json.loads(output.value)["station"]["description"], "Not a comment")
        menu = self.query("contextmenu", 0, 100, "playlist_index:0")
        self.assertIn("Station description: Not a comment", [row["text"] for row in menu["item_loop"]])

    def test_station_switch_clears_same_track_identity(self):
        self.start({"codec": "aac", "bitrate_kbps": 48})
        self.update(station={"id": "station:a", "name": "A"})
        self.update(station={"id": "station:b", "name": "B"}, track={"id": "a", "title": "New"})
        self.assertNotIn("type", self.current())
        self.assertEqual(self.current("N")["remote_title"], "B")

    def test_lossy_false_and_explicit_audio_clear(self):
        self.start({"codec": "mp3", "lossless": False})
        self.assertEqual(self.current()["lossless"], 0)
        self.update(track={"audio": None})
        self.assertNotIn("lossless", self.current())

    def test_pause_resume_and_lease_expiry_clear_extended_data(self):
        self.lib.np_destroy(self.manager)
        self.manager = self.lib.np_create(1, 8)
        _, claim, _ = self.api("claim", {"source": "test", "name": "Test"})
        self.token = claim["session_id"]
        self.start({"codec": "aac", "bitrate_kbps": 48})
        self.update(state="paused")
        self.assertEqual(self.current()["type"], "aac")
        self.update(state="playing")
        self.assertEqual(self.current()["bitrate"], "48kbps")
        time.sleep(1.1)
        self.assertEqual(self.lib.np_expire(self.manager), 1)
        self.assertEqual(self.query("status", "-", 1)["playlist_loop"], [])

    def test_songinfo_and_more_info_shapes(self):
        self.start({"codec": "aac", "bitrate_kbps": 48})
        fields = self.query("songinfo", 0, 100, "track_id:-1", "tags:or")["songinfo_loop"]
        self.assertTrue(all(len(field) == 1 for field in fields))
        self.assertEqual(dict(pair for field in fields for pair in field.items())["bitrate"], "48kbps")
        self.assertEqual(len(self.query("songinfo", 1, 1, "track_id:-1")["songinfo_loop"]), 1)
        menu = self.query("contextmenu", 0, 100, "menu:track", "context:playlist", "playlist_index:0")
        self.assertEqual(menu["count"], len(menu["item_loop"]))
        self.assertIn("Bitrate: 48kbps", [row["text"] for row in menu["item_loop"]])
        self.assertTrue(all(row["action"] == "none" and row["style"] == "itemNoAction" for row in menu["item_loop"]))
        self.assertEqual(self.query("trackinfo", "items", 0, 1, "track_id:-1")["item_loop"], menu["item_loop"][:1])
        self.assertEqual(self.query("songinfo", 0, 100, "track_id:other")["songinfo_loop"], [])

    def test_lifecycle_and_no_duplicate_changes(self):
        self.start({"codec": "mp3", "bitrate_kbps": 128})
        self.assertEqual(self.update(track={"audio": {"codec": "mp3", "bitrate_kbps": 128}})[2], 0)
        flags = self.update(track={"title": "Changed"})[2]
        self.assertEqual(flags, 2)  # metadata only, no ownership/playback/identity change
        self.update(track={"id": "b", "title": "Next"})
        self.assertNotIn("type", self.current())
        self.update(state="stopped")
        self.assertEqual(self.query("status", "-", 1)["playlist_loop"], [])
        self.api("release", {"session_id": self.token})
        self.assertEqual(self.query("contextmenu", 0, 100)["count"], 0)

    def test_owner_transfer(self):
        self.start({"codec": "flc", "lossless": True})
        _, claim, _ = self.api("claim", {"source": "other", "name": "Other"})
        old = self.token
        self.token = claim["session_id"]
        self.start()
        self.assertNotIn("lossless", self.current())
        self.assertEqual(self.api("update", {"session_id": old, "state": "playing"})[0], 409)

    def test_reject_invalid_atomically(self):
        self.start({"codec": "mp3"})
        for audio in [{"bitrate_kbps": -1}, {"bitrate_kbps": 1.5}, {"codec": "x" * 32},
                      {"lossless": "true"}, {"channels": 33}, {"replaygain_db": "bad"}, "bad"]:
            self.assertEqual(self.update(track={"title": "Must not commit", "audio": audio})[0], 400, repr(audio))
            self.assertEqual(self.current()["title"], "Song")
        self.assertEqual(self.update(station={"description": "x" * 256})[0], 400)
        self.assertEqual(self.update(track={"title": "x" * 17000})[0], 413)
        self.assertEqual(self.query("status", "-", 1, capacity=96),
                         {"error": "invalid_or_oversized_metadata_query"})

    def test_escape_and_remote_id(self):
        self.start({"codec": "aac"}, title='Title "\\\n café')
        self.assertEqual(self.current()["title"], 'Title "\\\n café')
        menu = self.query("status", "-", 1, "menu:menu")
        self.assertEqual(menu["item_loop"][0]["params"]["track_id"], -1)
        self.assertEqual(menu["base"]["actions"]["more"]["cmd"], ["contextmenu"])
        rows = self.query("contextmenu", 0, 100)["item_loop"]
        self.assertIn('Title: Title "\\\n café', [row["text"] for row in rows])


if __name__ == "__main__":
    unittest.main()
