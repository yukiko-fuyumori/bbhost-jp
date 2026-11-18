"""Steam layout routing tests; no Steam client or controller is needed."""

import importlib.util
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location("steam_wayland_input", Path(__file__).parents[1] / "tools/steam_wayland_input.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SteamWaylandInputTest(unittest.TestCase):
    def test_shortcut_and_regular_app_ids(self):
        self.assertEqual(module.steam_app_id({"SteamAppId": "3921653313"}), 3921653313)
        self.assertEqual(module.steam_app_id({"SteamGameId": "16843372725618606080"}), 3921653313)
        self.assertEqual(module.steam_app_id({"SteamAppId": "0", "SteamGameId": "620"}), 620)
        for environment in ({}, {"SteamAppId": "-1"}, {"SteamGameId": str(1 << 64)}, {"SteamAppId": "oops"}):
            with self.assertRaises(ValueError):
                module.steam_app_id(environment)

    def test_focus_loss_cancels_delayed_request_and_restores_desktop(self):
        requests = []
        called = threading.Event()

        def request(app):
            requests.append(app)
            called.set()
            return True

        bridge = module.FocusBridge(620, request, retry_delay=0.1)
        try:
            bridge.focus(True)
            self.assertTrue(called.wait(2))
            bridge.focus(False)
            time.sleep(0.2)
        finally:
            bridge.close()
        self.assertEqual(requests, [620, module.DESKTOP_APP_ID, 0])

    def test_focus_change_during_slow_request_cannot_leave_game_pinned(self):
        entered = threading.Event()
        release = threading.Event()
        requests = []

        def request(app):
            requests.append(app)
            if app == 620:
                entered.set()
                release.wait(2)
            return True

        bridge = module.FocusBridge(620, request)
        try:
            bridge.focus(True)
            self.assertTrue(entered.wait(2))
            bridge.focus(False)
            release.set()
        finally:
            release.set()
            bridge.close()
        self.assertEqual(requests, [620, module.DESKTOP_APP_ID, 0])

    def test_retry_once_then_cleanup_on_game_exit(self):
        requests = []
        retried = threading.Event()

        def request(app):
            requests.append(app)
            if requests.count(620) == 2:
                retried.set()
            return True

        bridge = module.FocusBridge(620, request, retry_delay=0.02)
        try:
            bridge.focus(True)
            self.assertTrue(retried.wait(2))
            time.sleep(0.08)
        finally:
            bridge.close()
        self.assertEqual(requests, [620, 620, module.DESKTOP_APP_ID, 0])

    def test_no_requests_when_game_never_takes_focus(self):
        requests = []
        bridge = module.FocusBridge(620, lambda app: requests.append(app))
        bridge.close()
        self.assertEqual(requests, [])

    def test_uncertain_request_still_gets_cleanup(self):
        requests = []
        called = threading.Event()

        def request(app):
            requests.append(app)
            called.set()
            return False

        bridge = module.FocusBridge(620, request)
        bridge.focus(True)
        try:
            self.assertTrue(called.wait(2))
        finally:
            bridge.close()
        self.assertEqual(requests, [620, module.DESKTOP_APP_ID, 0])

    def test_host_steam_cli_does_not_inherit_game_libraries(self):
        with mock.patch.dict(os.environ, {"LD_PRELOAD": "game-overlay", "LD_LIBRARY_PATH": "game-runtime"}):
            with mock.patch.object(module.subprocess, "run", return_value=mock.Mock(returncode=0)) as run:
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertTrue(module.request_layout(620))
        arguments, keywords = run.call_args
        self.assertEqual(arguments[0], ["steam", "-silent", "steam://forceinputappid/620"])
        self.assertNotIn("LD_PRELOAD", keywords["env"])
        self.assertNotIn("LD_LIBRARY_PATH", keywords["env"])
        self.assertEqual(keywords["timeout"], 3)

    def test_crashed_game_restores_layout_and_preserves_exit_status(self):
        with tempfile.TemporaryDirectory(prefix="bbhost-wayland-test-") as directory:
            root = Path(directory)
            requests = root / "requests.txt"
            steam = root / "steam"
            steam.write_text(f"#!{sys.executable}\nimport os,sys\n"
                             "with open(os.environ['TEST_REQUESTS'], 'a') as output:\n"
                             " output.write(sys.argv[-1] + '\\n')\n")
            steam.chmod(0o700)
            game = root / "fake game.py"
            game.write_text("import os,sys,time\n"
                            "assert os.environ['SDL_VIDEO_DRIVER'] == 'wayland'\n"
                            "assert os.environ['BBHOST_LOG'] == '1'\n"
                            "assert sys.argv[1] == 'argument with spaces'\n"
                            "print('[bbhost] window: keyboard focus gained', flush=True)\n"
                            "time.sleep(0.2)\n"
                            "os._exit(7)\n")
            environment = dict(os.environ, SteamAppId="620", TEST_REQUESTS=str(requests))
            environment["PATH"] = str(root) + os.pathsep + environment.get("PATH", "")
            result = subprocess.run([sys.executable, spec.origin, sys.executable, str(game), "argument with spaces"],
                                    env=environment, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertIn("[bbhost] window: keyboard focus gained", result.stdout)
            self.assertEqual(requests.read_text().splitlines(), ["steam://forceinputappid/620",
                             "steam://forceinputappid/413080", "steam://forceinputappid/0"])


if __name__ == "__main__":
    unittest.main()
