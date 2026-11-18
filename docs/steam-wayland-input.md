# Steam Input with native Wayland

Steam can detect a controller while failing to activate the game's layout for
a native Wayland window. This is tracked in
[Valve's issue tracker](https://github.com/ValveSoftware/steam-for-linux/issues/13314).
Running bbhost through XWayland (`SDL_VIDEO_DRIVER=x11 %command%`) is the
confirmed workaround for the Switch 2 Pro Controller on this machine.

`tools/steam_wayland_input.py` is an experimental alternative for native Linux
bbhost launched through native Steam. It requires Python 3 and the `steam`
command on PATH. Set the Steam shortcut's launch options to:

```text
python3 /home/Emi/bbhost-jp/tools/steam_wayland_input.py %command%
```

Use the absolute path to your checkout when installing elsewhere. The wrapper
selects SDL's Wayland backend and preserves the game's Steam environment. It
uses bbhost's existing focus messages to request the shortcut's Steam Input
layout, retries once after startup, and returns to the desktop layout on focus
loss. It clears the forced layout after restoring the desktop and when the game
exits, including a crash. Steam requests run separately from the game log reader.
The launcher remains controlled by keyboard and mouse.

Validated on 2026-11-07 with bbhost 0.2.26, KDE Wayland and a USB Switch 2 Pro
Controller: the user confirmed input at Bloodborne's title screen, and the
live logs confirmed game/desktop layout changes and override release on focus
loss. Eight automated checks cover layout selection, startup retry, focus races,
child environment handling and cleanup after a simulated game crash.

This uses Steam's undocumented, client-wide `forceinputappid` URL; it does not
implement the Steam overlay or recording on Wayland. Avoid using multiple such
wrappers simultaneously. If the wrapper itself is forcibly killed, close the
game and clear a leftover override from a terminal:

```sh
steam steam://forceinputappid/413080
steam steam://forceinputappid/0
```

To revert, restore `SDL_VIDEO_DRIVER=x11 %command%` in Steam's launch options.
Controller mappings, the USB initializer, and the game binary are unchanged.
