# Development Plan — overlay-timer

Translucent, click-through overlay timer pinned to a corner of one or more
screens, with countdown and pomodoro modes, controlled from the command line.

---

## 1. Overview

`overlay-timer` is a program written in **C** that draws a translucent text
(counter) on top of all windows, without receiving focus or clicks
(click-through), positioned in one of the 4 corners of one or more screens.

When invoked, the process **forks to the background** (daemonizes) and returns
the terminal immediately. Only **one instance** runs at a time. New invocations
talk to the running instance via a **Unix socket** to reconfigure, pause,
resume, stop or move the timer.

### Modes
- **Countdown**: counts down from a given time.
  On reaching zero: **blinks** and plays a **long sound**.
- **Pomodoro**: N pomodoros of 25 min (default color) with 5-min breaks
  (green). End of each pomodoro/break = **short beep**. The last pomodoro has no
  break afterward; its end plays a **long sound** and blinks. Optional long
  break (off by default).

---

## 2. Technical decisions (confirmed)

| Topic | Decision |
|---|---|
| Language | C (C11) |
| Build | CMake |
| Target distro | Debian |
| Windowing system | X11, Wayland, macOS (AppKit, see `PLAN_MACOS.md`) and Windows (Win32, see `PLAN_WINDOWS.md`); `--x11`/`--wayland` flag; autodetection via `XDG_SESSION_TYPE`/`WAYLAND_DISPLAY` |
| Wayland | Requires the **wlr-layer-shell** protocol (Sway/Hyprland/wlroots). Without layer-shell (e.g. GNOME): **not supported**, emits a warning and exits |
| Overlay | Low level: XCB (X11) / libwayland + layer-shell (Wayland) |
| Text rendering | **Pango + Cairo** (font family selection + alpha) |
| Audio | **libao + libsndfile** (plays `.wav`); fallback = **triangular-wave** tone generated at runtime |
| Config | simple **INI** file, **XDG** standard (`~/.config/overlay-timer/config.ini`) |
| IPC | **Unix domain socket** at `$XDG_RUNTIME_DIR/overlay-timer.sock` + lockfile/PID |
| Logs | `$XDG_STATE_HOME/overlay-timer/log` (stderr before daemonizing) |
| i18n | **GNU gettext** (en default, pt_BR); follows system locale |
| Docs | `--help` and a **man** page |

---

## 3. Single-instance behavior

1. On startup, acquires a lock (`flock` on a lockfile) and creates the socket.
2. If an instance **already exists**:
   - Control flags (`--pause`, `--resume`, `--stop`, `--corner`, `--screen`,
     color, alpha, size, font...) → sent to the daemon, which **reconfigures**
     the running timer.
   - `-t/--time` (start a new timer) or `--pomodoro` → **error**: ask to stop
     first (`overlay-timer --stop`).
3. `--corner`/`--screen` on a running timer only **move** the overlay.

---

## 4. Command-line interface

```
overlay-timer [options]

Time / mode:
  -t, --time <MM:SS | HH:MM:SS>   start countdown
      --pomodoro <N>              start N pomodoros
      --format <hhmmss|mmss>      display format (time takes precedence)

Appearance:
  -c, --color <color>             text color (name or #RRGGBB)
      --alpha <0-100>             visibility (0=transparent, 100=opaque)
      --size <0-100>              % of the smaller screen dimension for the text
      --font <family>             font family (fontconfig)
      --corner <tl|tr|bl|br>      corner
      --screen <idx[,idx...]>     screen index(es)

Pomodoro:
      --pomodoro-minutes <min>
      --break-minutes <min>
      --long-break-minutes <min>
      --long-break-every <N>      0 = no long break (default)
      --pomodoro-color <color>
      --break-color <color>

Control (requires a running instance):
      --pause | --resume | --stop

Windowing system:
      --x11 | --wayland

Config:
      --config <path>             use an alternative file
      --save                      merge the current options into the config and write

General:
  -h, --help
  -V, --version
```

---

## 5. Positioning, size and appearance

- **Text size** = `size%` of the screen's **smaller dimension**. The window box
  fits the text; the background is **fully transparent**.
- **Click-through**: undecorated window, no focus, empty input region
  (X11: empty shape input; Wayland layer-shell: `set_keyboard_interactivity
  none` + empty input region).
- **Visibility (alpha)**: applied only to the **text** (0=invisible, 100=opaque).
- **Multi-screen**: indexes; the **same** timer is replicated on each chosen screen.
- **Pomodoro label** (e.g. `2/4`): small text positioned relative to the counter
  according to the corner:
  - counter on top → label **below** it; at the bottom → label **above**.
  - left corner → **left**-aligned; right → **right**-aligned.

---

## 6. Audio

- Sounds in a configurable folder. Expected default names:
  - `timer_end.wav`, `pomodoro_end.wav`, `break_end.wav`
- No file → synthesized **triangular** tone:
  - end of timer / last pomodoro → **long** tone
  - end of pomodoro / end of break → **short** tone
- Playback: libsndfile reads the `.wav`, libao plays it.

---

## 7. Configuration (INI) — default keys

See `config/config.ini`. Created at `~/.config/overlay-timer/config.ini` on
first run, if it does not exist. `--save` merges.

---

## 8. Folder structure

```
overlay-timer/
├── CMakeLists.txt
├── PLAN.md
├── README.md
├── LICENSE
├── config/
│   └── config.ini              # default configuration template
├── sounds/                     # .wav sounds (filled in by the user)
│   └── .gitkeep
├── po/                         # gettext catalogs (en, pt_BR)
│   ├── overlay-timer.pot
│   ├── en.po
│   └── pt_BR.po
├── man/
│   └── overlay-timer.1         # manual page
├── src/
│   ├── main.c                  # arg parsing, daemonize, dispatch
│   ├── config.c / config.h     # INI read/write + merge + XDG
│   ├── cli.c / cli.h           # flag definition/parsing, help
│   ├── timer.c / timer.h       # countdown and pomodoro logic (loop)
│   ├── ipc.c / ipc.h           # Unix socket, lockfile/PID, protocol
│   ├── audio.c / audio.h       # libao+libsndfile, triangular fallback
│   ├── render.c / render.h     # Pango/Cairo: draws text + label
│   ├── backend.h               # abstract window backend interface
│   ├── backend_x11.c           # XCB + empty shape input
│   ├── backend_wayland.c       # libwayland + wlr-layer-shell
│   ├── backend.c               # backend selection/autodetection
│   ├── i18n.h                  # gettext macros (_())
│   └── util.c / util.h         # helpers (time/color parsing, XDG paths)
└── tests/
    └── ...                     # time/color/config parsing tests
```

---

## 9. Execution architecture

```
CLI invocation
   │
   ├─ parse args ──► is it a control command? ─yes─► connect socket ─► send ─► exit
   │                                   │
   │                                  no
   │                                   ▼
   ├─ instance already exists? ─yes─► (control) reconfig / (new timer) error
   │                                   │
   │                                  no
   │                                   ▼
   ├─ daemonize (fork, setsid, close fds) ─► return terminal
   │
   └─ [background]
        ├─ create lockfile + socket
        ├─ select backend (x11/wayland)
        ├─ create overlay(s) per screen
        ├─ main loop:
        │    ├─ timer tick (countdown/pomodoro)
        │    ├─ render (Pango/Cairo)
        │    ├─ process IPC messages (pause/resume/stop/move/reconfig)
        │    └─ end events → audio + blink
        └─ cleanup (remove socket/lock) on stop
```

### IPC protocol (plain text over socket)
Line-by-line messages: `PAUSE`, `RESUME`, `STOP`, `SET corner=br`,
`SET screen=0,1`, `SET color=#00ff00`, etc. The daemon replies `OK`/`ERR <msg>`.

---

## 10. Dependencies (Debian)

- `cmake`, `build-essential`, `pkg-config`
- `libxcb1-dev`, `libxcb-shape0-dev`, `libxcb-randr0-dev` (X11)
- `libwayland-dev`, `wayland-protocols` + `wlr-layer-shell` source (Wayland)
- `libcairo2-dev`, `libpango1.0-dev` (text)
- `libao-dev`, `libsndfile1-dev` (audio)
- `gettext` (i18n)

CMake with options `-DWITH_X11=ON/OFF` and `-DWITH_WAYLAND=ON/OFF` to choose,
at compile time, which backends to build (at least one required).

---

## 11. Development phases

1. **Skeleton**: CMake, arg parsing, `--help`, INI config (read/write/merge
   + default creation), util (time/color parsing, XDG paths). No overlay yet.
2. **Daemon/IPC**: fork/daemonize, lockfile, socket, protocol, control commands
   (pause/resume/stop/move) with a stub "render" to stdout.
3. **X11 backend**: click-through XCB window + Pango/Cairo + positioning in the
   4 corners + multi-screen (RandR).
4. **Timer logic**: countdown + hh:mm:ss/mm:ss format + blink.
5. **Audio**: libao+libsndfile + triangular fallback.
6. **Pomodoro**: sequence, colors, N/total label, breaks.
7. **Wayland backend**: layer-shell; warning when not supported.
8. **i18n**: gettext, en/pt_BR catalogs.
9. **Docs**: man page + README.
10. **Tests**: time/color/config parsing.

---

## 12. Resolved points

- i18n: **gettext**, languages **en** (default) and **pt_BR**, following the
  locale. ✅
- Default sounds folder: `~/.local/share/overlay-timer/sounds/` (XDG data),
  overridable via `sounds_dir` in the INI. ✅

## 13. Progress

- [x] **Phase 1** — Skeleton: CMake, arg parsing, `--help`, INI config
  (read/write/merge + default creation), util (time/color parsing, XDG paths).
- [x] **Phase 2** — Daemon/IPC: fork/daemonize, single-instance lockfile,
  Unix socket, message protocol, control commands
  (pause/resume/stop/reconfigure/status), event loop with a 1s tick.
- [x] **Phase 3** — X11 backend: click-through ARGB XCB window (empty SHAPE
  input), rendering with Pango/Cairo, positioning in the 4 corners, multi-screen
  via RandR, backend autodetection, integration into the daemon loop, blink at
  the end.
- [x] **Phase 4** — Timer logic: `timer.c` module (state machine) with
  countdown and full pomodoro (work/break/long-break phases, N/total label,
  transition events), hh:mm:ss/mm:ss format, unit tests.
- [x] **Phase 5** — Audio: `audio.c` module (libao + libsndfile) in its own
  thread; plays timer_end/pomodoro_end/break_end.wav; synthesized triangular
  tone fallback (long/short); wired to the daemon events.
- [x] **Phase 6** — Pomodoro: validated end to end in the real daemon
  (work→break→work sequence, per-phase colors, N/total label, short sound on
  transitions, long sound + blink on the last pomodoro, no break after the
  last). Durations and colors configurable via CLI/INI.
- [x] **Phase 7** — Wayland backend: vtable architecture (`backend_internal.h`)
  so X11 and Wayland coexist in the same binary without symbol collisions;
  `backend_wayland.c` with wlr-layer-shell (anchored to the edges, empty input =
  click-through, `keyboard_interactivity=none`, `exclusive_zone=-1`); SHM
  buffers via memfd + cairo; wlr-layer-shell protocol vendored in
  `protocols/` and generated with `wayland-scanner` in CMake; clear warning when
  the compositor lacks layer-shell (e.g. GNOME). Builds with both backends,
  tests pass, X11 revalidated in a real session.
- [x] **Phase 8** — i18n: string extraction with `xgettext`
  (`po/overlay-timer.pot`), catalogs `po/pt_BR.po` (67 translated messages) and
  `po/en.po` (fallback to the English original); `.mo` generation via
  `msgfmt` in CMake (`translations` target, installed in
  `share/locale/<lang>/LC_MESSAGES`). Validated at runtime: `--help` and error
  messages come out in pt_BR with `LC_ALL=pt_BR.utf8` and in English in the
  default locale.
- [x] **Phase 9** — Docs: complete `man/overlay-timer.1` manual page
  (NAME/SYNOPSIS/DESCRIPTION/OPTIONS/BEHAVIOR/CONFIGURATION/FILES/
  ENVIRONMENT/EXIT STATUS/EXAMPLES/SEE ALSO sections), validated with `man`
  without warnings and installed by CMake in `share/man/man1`; README finalized
  (per-backend build, installation, tests, backends, configuration, languages).
- [x] **Phase 10** — Tests: coverage expanded from 3 to 5 suites.
  `test_cli.c` covers the CLI parser (countdown, pomodoro, control,
  help/version, appearance overrides, invalid values, backend/sound flags,
  RECONFIGURE action and serialization of the `SET` IPC message);
  `test_config_kv.c` covers the enum conversions (corner/format/backend),
  `config_set_kv` (strings, numbers with range, enums, bool, screen list,
  invalid keys/values) and the deep copy of config. All 5 suites pass (`ctest`).
- [x] **Phase 11** — macOS port (see `PLAN_MACOS.md`): `backend_macos.m`
  (AppKit, click-through transparent window per screen, Cairo → CGImage with
  Retina scale, non-blocking event pump); `BACKEND_MACOS` + `macos` config
  value; daemon pumps fd-less backends every iteration; portable
  `FD_CLOEXEC` sockets; `$TMPDIR` runtime dir on macOS; CMake `WITH_MACOS`,
  `Intl` and pkg-config imported targets. Linux build and tests revalidated;
  **still needs validation on a real Mac**.
- [x] **Phase 12** — Windows port (see `PLAN_WINDOWS.md`): `backend_win32.c`
  (layered click-through topmost window per monitor, Cairo DIB →
  `UpdateLayeredWindow`); `ipc_win32.c` (Winsock `AF_UNIX`, named mutex,
  self-relaunch `daemonize()`); `WSAPoll` in the daemon; `%APPDATA%` /
  `%LOCALAPPDATA%` paths; `FOPEN_CLOEXEC`, `str_tok`, `local_time` helpers;
  `BACKEND_WIN32` + `win32` config value; CMake `WITH_WIN32`. Linux build and
  tests revalidated; MinGW-w64 cross-build checked; **still needs validation
  on real Windows**.
