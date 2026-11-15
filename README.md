# overlay-timer

Translucent, *click-through* overlay timer pinned to a corner of one or more
screens. Supports **countdown** and **pomodoro mode**. Written in C, with
backends for **X11** (XCB) and **Wayland** (wlr-layer-shell).

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
overlay-timer --pause             # pause the running timer
overlay-timer --corner tl         # move to the top-left corner
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

The backend is autodetected from `XDG_SESSION_TYPE`/`WAYLAND_DISPLAY`/`DISPLAY`,
or can be forced with `--x11` / `--wayland`.

## Configuration

INI file at `~/.config/overlay-timer/config.ini` (created with default values
on first run). CLI options take precedence; use `--save` to write them back.
See [`config/config.ini`](config/config.ini) for the commented template.

## Languages

The interface follows the system locale. **English** (default) and
**Brazilian Portuguese** are available:

```sh
LC_ALL=pt_BR.UTF-8 overlay-timer --help
```
