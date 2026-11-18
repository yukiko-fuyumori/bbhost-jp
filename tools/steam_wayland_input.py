#!/usr/bin/env python3
"""Run native-Wayland bbhost with Steam Input following its window focus."""

import argparse
import os
import signal
import subprocess
import sys
import threading
import time


DESKTOP_APP_ID = 413080


def steam_app_id(environ):
    """Non-Steam shortcuts use the upper 32 bits of SteamGameId."""
    try:
        app_id = int(environ.get("SteamAppId", "0"), 10)
        if 0 < app_id <= 0xFFFFFFFF:
            return app_id
        game_id = int(environ.get("SteamGameId", "0"), 10)
        if 0 < game_id <= 0xFFFFFFFFFFFFFFFF:
            return game_id >> 32 if game_id > 0xFFFFFFFF else game_id
    except ValueError:
        pass
    raise ValueError("Start this wrapper through the bbhost Steam shortcut (SteamAppId is missing or invalid).")


class FocusBridge:
    """Serialize Steam requests off the game's output reader thread."""

    def __init__(self, app_id, request, retry_delay=4.0):
        self.app_id = app_id
        self.request = request
        self.retry_delay = retry_delay
        self.condition = threading.Condition()
        self.focused = False
        self.generation = 0
        self.stopping = False
        self.thread = threading.Thread(target=self._run, name="steam-input-focus")
        self.thread.start()

    def focus(self, focused):
        with self.condition:
            if self.focused != focused:
                self.focused = focused
                self.generation += 1
                self.condition.notify()

    def close(self):
        with self.condition:
            self.stopping = True
            self.condition.notify()
        self.thread.join()

    def _run(self):
        seen = 0
        pinned = False
        retry_at = None
        while True:
            with self.condition:
                while not self.stopping and seen == self.generation:
                    timeout = None if retry_at is None else retry_at - time.monotonic()
                    if timeout is not None and timeout <= 0:
                        break
                    self.condition.wait(timeout)
                stopping = self.stopping
                focused = self.focused and not stopping
                changed = seen != self.generation
                seen = self.generation
            if focused:
                # A new window can override the first request during startup.
                # Retry once after it has settled; focus loss cancels the retry.
                # A timed-out CLI can still have delivered the URL. Always
                # attempt cleanup after a request, including an uncertain one.
                pinned = True
                self.request(self.app_id)
                retry_at = time.monotonic() + self.retry_delay if changed else None
            else:
                retry_at = None
                if pinned:
                    # Clearing alone leaves the previous layout active until
                    # another X11 focus event, which Wayland doesn't supply.
                    self.request(DESKTOP_APP_ID)
                    self.request(0)
                    pinned = False
            if stopping:
                return


def request_layout(app_id):
    environment = os.environ.copy()
    # Steam's overlay/runtime libraries belong to the game, not this host CLI.
    for name in ("LD_PRELOAD", "LD_LIBRARY_PATH"):
        environment.pop(name, None)
    try:
        result = subprocess.run(
            ["steam", "-silent", f"steam://forceinputappid/{app_id}"],
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=3,
            check=False,
        )
        if result.returncode == 0:
            print(f"[bbhost-wayland-input] layout {app_id}", file=sys.stderr, flush=True)
            return True
        detail = f"Steam exited with status {result.returncode}"
    except (OSError, subprocess.TimeoutExpired) as error:
        detail = str(error)
    print(f"[bbhost-wayland-input] layout request failed: {detail}", file=sys.stderr, flush=True)
    return False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs=argparse.REMAINDER, help="bbhost executable and its arguments")
    arguments = parser.parse_args(argv)
    command = arguments.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("supply the bbhost command; use this wrapper before %command% in Steam")
    try:
        app_id = steam_app_id(os.environ)
    except ValueError as error:
        parser.error(str(error))
    environment = os.environ.copy()
    environment["SDL_VIDEO_DRIVER"] = "wayland"
    # Keep focus messages even when the player's tooling/logging is disabled.
    # This does not turn the performance instrumentation on.
    environment["BBHOST_LOG"] = "1"
    try:
        child = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, errors="replace", bufsize=1)
    except OSError as error:
        parser.exit(1, f"Could not start bbhost: {error}\n")

    def forward_signal(number, _frame):
        if child.poll() is None:
            try:
                child.send_signal(number)
            except ProcessLookupError:
                pass  # The game exited between poll() and signal delivery.

    old_handlers = {number: signal.signal(number, forward_signal) for number in (signal.SIGINT, signal.SIGTERM)}
    bridge = FocusBridge(app_id, request_layout)
    try:
        for line in child.stdout:
            message = line.rstrip()
            if message == "[bbhost] window: keyboard focus gained":
                bridge.focus(True)
            elif message == "[bbhost] window: keyboard focus lost":
                bridge.focus(False)
            try:
                sys.stdout.write(line)
                sys.stdout.flush()
            except BrokenPipeError:
                # Continue draining the child's log and watching its lifetime.
                pass
        return_code = child.wait()
    finally:
        bridge.close()
        child.stdout.close()
        for number, handler in old_handlers.items():
            signal.signal(number, handler)
    return return_code if return_code >= 0 else 128 - return_code


if __name__ == "__main__":
    sys.exit(main())
