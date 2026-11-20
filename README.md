# overlay-timer

Translucent, *click-through* overlay timer pinned to a corner of one or more
screens. Supports **countdown** and **pomodoro mode**. Written in C, with
backends for **X11** (XCB), **Wayland** (wlr-layer-shell), **macOS** (AppKit) and **Windows** (10 or newer).

> See [`PLAN.md`](PLAN.md) for the full development plan, architecture and
> technical decisions.

## Features
- Translucent, undecorated overlay that receives neither focus nor clicks.
- Positionable in any of the 4 corners; replicable across multiple screens.
- Configurable color, visibility (alpha), size (% of the screen), font and format.
- Configuration in an **INI** file (XDG standard), overridable via CLI; `--save`
  writes it back, merging.
- Single instance with socket control: `--pause`, `--resume`, `--stop`,
  move (`--corner`/`--screen`).
- Audio via libao + libsndfile, with a synthesized triangular-tone fallback.
- Internationalization (gettext: English and Brazilian Portuguese) and a `man` page.

## Build (Debian/Ubuntu)
```sh
sudo apt install cmake build-essential pkg-config \
  libxcb1-dev libxcb-shape0-dev libxcb-randr0-dev \
  libwayland-dev wayland-protocols \
  libcairo2-dev libpango1.0-dev \
  libao-dev libsndfile1-dev gettext

cmake -B build -DWITH_X11=ON -DWITH_WAYLAND=ON
cmake --build build
```

Both backends are built by default. To build only one:

```sh
cmake -B build -DWITH_X11=ON -DWITH_WAYLAND=OFF   # X11 only
cmake -B build -DWITH_X11=OFF -DWITH_WAYLAND=ON   # Wayland only
```

At least one backend is required.

### Build (macOS)

```sh
brew install cmake pkg-config cairo pango libao libsndfile gettext

cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build
```

On macOS only the native AppKit backend is built by default (`WITH_MACOS=ON`,
`WITH_X11=OFF`, `WITH_WAYLAND=OFF`).

### Build (Windows)

Windows 10 (1803) or newer, built with [MSYS2](https://www.msys2.org/). In the
**UCRT64** shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,pkgconf,cairo,pango,libao,libsndfile,gettext}

cmake -B build -G Ninja
cmake --build build
```

On Windows only the native Win32 backend is built by default (`WITH_WIN32=ON`).
The executable depends on the MSYS2 DLLs; run it from the UCRT64 shell or copy
the DLLs next to it.

### Installation

```sh
sudo cmake --install build
```

This installs the executable, the `man` page (`man overlay-timer`) and the
translation catalogs (`.mo`).

### Tests

```sh
ctest --test-dir build --output-on-failure
```

## Quick start
```sh
overlay-timer -t 25:00            # start a 25-min countdown
overlay-timer --pomodoro 4        # 4 pomodoros
overlay-timer --pomodoro 4 --sync # 4 pomodoros aligned with :00 and :30
overlay-timer --pause             # pause the running timer
overlay-timer --corner tl         # move to the top-left corner
overlay-timer --screen 0,1        # show on screens 0 and 1 (works while running)
overlay-timer --stop              # stop
```

The process daemonizes and returns the terminal immediately; only one instance
runs at a time. See `overlay-timer --help` or `man overlay-timer` for all
options.

## Backends

- **X11** (XCB): 32-bit ARGB window, click-through via SHAPE with an empty
  input region, multi-monitor via RandR. Requires an active compositor for
  transparency.
- **Wayland** (wlr-layer-shell): requires a **wlroots**-based compositor
  (Sway, Hyprland, ...). Compositors without the `wlr-layer-shell` protocol
  (e.g. GNOME/Mutter) are **not supported** and produce a clear error.

- **macOS** (AppKit): one borderless, transparent window per screen that
  ignores the mouse, stays above normal windows and on every Space (including
  over full-screen apps). No Dock icon. The control socket lives in
  `$TMPDIR/overlay-timer/`. Font family names differ from Linux (e.g. `Menlo`).

- **Windows** (Win32): one layered, click-through, topmost window per screen
  (work area, above the taskbar), hidden from the taskbar and Alt+Tab.
  Config in `%APPDATA%\overlay-timer\`, log and control socket in
  `%LOCALAPPDATA%\overlay-timer\`. Font family names differ from Linux
  (e.g. `Consolas`).

### Multiple screens and hot-plug

`--screen` takes a list of monitor indices (in the order the window system
lists them; 0 is the primary monitor on X11/macOS/Windows and the first
advertised output on Wayland). It can be changed while the timer runs; the
reply says where the overlay actually is, e.g.
`OK 1 applied; showing on screen(s) 0 (1 monitor(s)); unavailable: 1`.

Monitors are watched while the timer runs (RandR events on X11, output
add/remove on Wayland, `WM_DISPLAYCHANGE` on Windows, CoreGraphics display
reconfiguration on macOS). When a monitor goes away, its overlay is removed
instead of being moved onto (and stacked over) another screen; when it comes
back, the overlay is recreated on it. If none of the requested screens exist,
the overlay falls back to screen 0; with no monitor at all the daemon keeps
running and waits. Indices are positional, so after a reconnect a monitor
may get a different index (on Wayland, a re-added output goes to the end).

The window system itself (`--x11`/`--wayland`) cannot be changed while a
timer is running.

On Linux the backend is autodetected from
`XDG_SESSION_TYPE`/`WAYLAND_DISPLAY`/`DISPLAY`, or can be forced with
`--x11` / `--wayland`. On macOS the AppKit backend is always used; on
Windows, the Win32 backend.

## Configuration

INI file at `~/.config/overlay-timer/config.ini` (on Windows,
`%APPDATA%\overlay-timer\config.ini`; created with default values
on first run). CLI options take precedence; use `--save` to write them back.
See [`config/config.ini`](config/config.ini) for the commented template.

## Languages

The interface follows the system locale. **English** (default) and
**Brazilian Portuguese** are available:

```sh
LC_ALL=pt_BR.UTF-8 overlay-timer --help
```
