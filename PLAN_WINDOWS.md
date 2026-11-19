# Windows Port Plan — overlay-timer

> **Status:** implemented (steps 1–5 and 7). Step 6 (validation on real
> Windows) is pending: the code was built and tested on Linux, and the
> Windows sources were cross-compiled and linked with MinGW-w64 (audio
> excluded, since libsndfile is not packaged for the cross toolchain).

## 1. Feasibility

**Verdict: feasible with medium effort.** The window backend is isolated
behind the vtable (`backend_internal.h`) and the daemon already supports
backends without a pollable fd (done for macOS). Most of the work is in the
POSIX parts: daemonizing, the single-instance lock and the socket.

Guiding rule: **keep it as simple as possible.**

- Build only with **MSYS2 / MinGW-w64** (UCRT64 environment). This gives us
  `getopt_long`, `clock_gettime`, pthreads (winpthreads), pkg-config and every
  library through `pacman`. MSVC is out of scope.
- Keep the existing IPC protocol and **keep using `AF_UNIX` sockets**. Windows
  10 (1803+) supports them through Winsock, so the client/server logic stays
  nearly identical.
- Copy what already works on macOS: one window per screen, `get_fd() == -1`,
  pump on every loop iteration.

| Area | Status on Windows | Work needed |
|---|---|---|
| CLI (`getopt_long`) | Provided by MinGW-w64 | None |
| Timer, render (Cairo + Pango) | Portable; libraries in MSYS2 | None |
| Audio (libao + libsndfile) | Available in MSYS2; libao has a `wmm` driver | None |
| i18n (gettext) | `libintl` in MSYS2; `find_package(Intl)` already used | None |
| `fopen(..., "re"/"we"/"ae")` | UCRT **rejects** the `e` flag (`fopen` returns NULL) | Small fix |
| `util.c` (home, XDG, `mkdir`) | No `getpwuid`, no `mkdir(path, mode)`, drive letters | Small `#ifdef _WIN32` |
| `daemon.c` (`poll`, `sigaction`, `SIGPIPE`) | Use `WSAPoll`, `signal()` | Small `#ifdef _WIN32` |
| IPC (`ipc.c`) | No `fork`, `flock`, `/dev/null`; sockets are not fds | **New `ipc_win32.c`** (~200 lines, mostly copied) |
| Window backend | — | **New `backend_win32.c`** (~200 lines) |

Not touched: `backend_x11.c`, `backend_wayland.c`, `backend_macos.m`,
`render.c`, `timer.c`, `audio.c`, `cli.c`, `ipc.h`, `backend.h`.

---

## 2. Design

### 2.1 New backend: `src/backend_win32.c` (Win32 + cairo-win32)

Same model as X11/macOS: **one transparent, full-work-area window per
selected screen**, drawn with the existing `render_frame()`.

- **Init**
  - `SetProcessDPIAware()` so sizes are in real pixels (the text size is a %
    of the screen, so physical pixels are what we want).
  - `RegisterClassW` with `DefWindowProcW` as the window procedure (the window
    takes no input, so no custom handling is needed).
  - Screens: `EnumDisplayMonitors` + `GetMonitorInfoW`. Put the primary
    monitor (`MONITORINFOF_PRIMARY`) at index 0, the others in enumeration
    order. Use `rcWork` so the text does not sit under the taskbar.
- **Window per screen**: `CreateWindowExW` with
  - `WS_EX_LAYERED` → per-pixel alpha
  - `WS_EX_TRANSPARENT` → **click-through**
  - `WS_EX_TOPMOST` → above normal windows
  - `WS_EX_TOOLWINDOW` → not in the taskbar or Alt+Tab
  - `WS_EX_NOACTIVATE` → never takes focus
  - style `WS_POPUP`; show with `ShowWindow(hwnd, SW_SHOWNOACTIVATE)`.
- **Render**
  - Per window, keep a `cairo_win32_surface_create_with_dib(CAIRO_FORMAT_ARGB32,
    w, h)` surface and its `cairo_t`.
  - `render_frame()` → `cairo_surface_flush()` → `UpdateLayeredWindow(hwnd,
    NULL, &pos, &size, cairo_win32_surface_get_dc(surface), &(POINT){0,0}, 0,
    &blend, ULW_ALPHA)` with
    `blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA }`.
    Cairo's ARGB32 is premultiplied BGRA, which is exactly what
    `UpdateLayeredWindow` expects. No `WM_PAINT` handling needed.
- **Vtable**
  - `get_fd` → `-1`.
  - `pump` → `while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    { TranslateMessage; DispatchMessageW; }`.
  - `destroy` → `DestroyWindow`, destroy Cairo objects, `UnregisterClassW`.
  - `name` → `"win32"`.

### 2.2 Backend selection

Same pattern as `BACKEND_MACOS`:

- `config.h`: add `BACKEND_WIN32`.
- `config.c`: accept/emit `"win32"` for the `backend` key.
- `backend.c`: with `WITH_WIN32`, `detect_backend()` returns `BACKEND_WIN32`;
  dispatch to `backend_create_win32()`.
- `backend_internal.h`: declare `backend_create_win32()` under `WITH_WIN32`.
- `tests/test_config_kv.c`: add the `"win32"` round-trip check.

### 2.3 IPC: new `src/ipc_win32.c`

Implements the **same `ipc.h` API**; CMake compiles either `ipc.c` or
`ipc_win32.c`. Start by copying `ipc.c` and change only:

- **Sockets**: `#include <winsock2.h>` + `<afunix.h>`. Call
  `WSAStartup(MAKEWORD(2,2), ...)` once (lazily, in a small `ensure_wsa()`).
  Replace `read`/`write`/`close` on sockets with `recv`/`send`/`closesocket`.
  Store the `SOCKET` in the existing `int listen_fd` (Windows socket handles
  fit in 32 bits); this way `ipc.h` is not changed.
- **Single instance**: instead of `flock`, `CreateMutexW(NULL, TRUE,
  L"Local\\overlay-timer")`; if `GetLastError() == ERROR_ALREADY_EXISTS`,
  return `1`. Keep the handle in `lock_fd` (cast) or a static `HANDLE`.
  No lockfile is needed.
- **Orphan socket**: `DeleteFileA(sock_path)` before `bind` (same as
  `unlink`).
- **`daemonize()`**: there is no `fork`, so the program **re-launches
  itself**:
  - If the environment variable `OVERLAY_TIMER_DAEMON=1` is set → we are
    the child: return `true`.
  - Otherwise: set `OVERLAY_TIMER_DAEMON=1`, call `CreateProcessW(NULL,
    GetCommandLineW(), ..., DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, ...)`
    (the child inherits the environment and the same arguments), then
    `exit(0)` in the parent.
  - `DETACHED_PROCESS` means the child has no console, so no window pops up
    and stdout/stderr writes are silently dropped (same as `/dev/null`).

  Side effects to accept (harmless): the child parses the CLI again and, with
  `--save`, writes the same config a second time.

### 2.4 Daemon loop (`daemon.c`)

Small `#ifdef _WIN32` block at the top:

```c
#ifdef _WIN32
#include <winsock2.h>
#define poll(f, n, t) WSAPoll((f), (n), (t))
#else
#include <poll.h>
#endif
```

- `WSAPoll` ignores entries with a negative `fd`, so the backend slot with
  `-1` is safe; the existing "pump every iteration when `fd < 0`" line already
  covers Win32.
- Signals: replace `sigaction` with `signal(SIGINT/SIGTERM, on_signal)` under
  `_WIN32`; skip `SIGPIPE` (does not exist). In practice the daemon is stopped
  via `--stop` (IPC), so this is just a fallback.
- The loop wakes at least every second, well under the 5 s after which
  Windows marks a window as "Not responding".

### 2.5 Small portability fixes

- **`fopen` modes** (`util.c`, `config.c`): the `e` flag makes UCRT's `fopen`
  fail. Add to `util.h`:

  ```c
  #ifdef _WIN32
  #define FOPEN_CLOEXEC ""
  #else
  #define FOPEN_CLOEXEC "e"
  #endif
  ```
  and use `fopen(path, "r" FOPEN_CLOEXEC)` etc. (4 places).
- **`util.c`** under `#ifdef _WIN32`:
  - `home_dir()`: `getenv("USERPROFILE")`.
  - Paths: config in `%APPDATA%\overlay-timer\config.ini`; state (log) and
    runtime (socket) in `%LOCALAPPDATA%\overlay-timer`. Convert `\` to `/`
    when reading these variables (Windows accepts `/`), so the rest of the
    code that joins paths with `/` keeps working.
  - Accept absolute paths like `C:/...` (today the code checks `v[0] == '/'`).
  - `mkdirs()`: use `_mkdir(path)` (no mode) and skip the drive prefix
    (`C:`) before creating directories.
  - Drop `<pwd.h>` and `getpwuid` on Windows.
- `strtok_r` / `localtime_r`: provided by MinGW-w64 headers; if a build
  complains, map them to `strtok_s` / `localtime_s`.

### 2.6 Build (`CMakeLists.txt`)

```cmake
option(WITH_WIN32 "Build the Windows (Win32) backend" ${WIN32})
# On WIN32, default WITH_X11 / WITH_WAYLAND to OFF (like APPLE).
# The "at least one backend" check includes WITH_WIN32.

if(WIN32)
    list(REMOVE_ITEM CORE_SOURCES src/ipc.c)
    list(APPEND CORE_SOURCES src/ipc_win32.c)
endif()           # must run before add_library(overlay_timer_core ...)

if(WITH_WIN32)
    if(NOT WIN32)
        message(FATAL_ERROR "WITH_WIN32 requires building on Windows")
    endif()
    pkg_check_modules(CAIRO_WIN32 REQUIRED IMPORTED_TARGET cairo-win32)
    target_sources(overlay_timer_core PRIVATE src/backend_win32.c)
    target_compile_definitions(overlay_timer_core PUBLIC WITH_WIN32
        _WIN32_WINNT=0x0A00)              # Windows 10 (afunix, WSAPoll)
    target_link_libraries(overlay_timer_core PUBLIC
        PkgConfig::CAIRO_WIN32 ws2_32 user32 gdi32)
endif()
```

- Keep the console subsystem (no `-mwindows`) so `--help`, `--status` etc.
  print to the terminal; the daemon has no console because of
  `DETACHED_PROCESS`.
- The `-Wall -Wextra -Wshadow` flags and `-D_GNU_SOURCE` are fine with
  MinGW GCC.

### 2.7 Dependencies (MSYS2, UCRT64 shell)

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,pkgconf,cairo,pango,libao,libsndfile,gettext}
```

Build:

```sh
cmake -B build -G Ninja
cmake --build build
```

---

## 3. Steps

1. **Portability fixes**: `FOPEN_CLOEXEC`, `util.c` paths/`mkdirs`, CMake
   options. Verify it still builds and all tests pass on Linux.
2. **Backend selection**: `BACKEND_WIN32` enum, config string, autodetection,
   constructor declaration, test update.
3. **`ipc_win32.c`**: AF_UNIX via Winsock, named mutex, self-relaunch
   `daemonize()`.
4. **`daemon.c`**: `WSAPoll` alias and `signal()` under `_WIN32`.
5. **`backend_win32.c`**: layered windows per monitor, Cairo DIB surface,
   `UpdateLayeredWindow`, `PeekMessage` pump.
6. **Validate on Windows 10/11**: build, `ctest`, then manually check
   countdown, pomodoro, the 4 corners, multi-screen, pause/resume/stop/move
   via IPC, sound, blink, `--help` in pt_BR, click-through, and that the
   overlay does not appear in the taskbar or Alt+Tab.
7. **Docs**: README (MSYS2 build section), man page (`win32` value for
   `backend`, Windows paths), PLAN.md progress entry; update `po/` if new
   user-visible strings were added.

---

## 4. Known limitations / risks

- **Windows 10 1803+ only** (AF_UNIX support). Older versions are out of
  scope.
- **Exclusive full-screen games/apps** may draw over `WS_EX_TOPMOST`
  windows; borderless full-screen apps are fine.
- **Translations**: `LOCALEDIR` is an absolute path fixed at build time, so
  `.mo` files are found only when installed into that prefix (e.g. inside
  MSYS2). Making it relative to the executable is a possible later
  improvement. gettext picks the language from `LANG`/`LANGUAGE` or the user
  locale.
- **Distribution**: the `.exe` depends on MSYS2 DLLs (cairo, pango, glib…).
  For use outside MSYS2 they would need to be copied next to the executable
  (e.g. with `ldd` + `cp`). Installer/signing is out of scope.
- **Screen changes** (monitor plugged/unplugged, DPI change) are not handled,
  same as the other backends.
- **Fonts**: Pango on Windows uses the system fonts; names differ from Linux
  (e.g. `Consolas`, `Segoe UI`).
