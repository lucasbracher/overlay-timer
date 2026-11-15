# macOS Port Plan — overlay-timer

> **Status:** implemented (steps 1–4 and 6). Step 5 (validation on a real
> Mac) is pending: the code was only built and tested on Linux.

## 1. Feasibility

**Verdict: feasible with low effort.** The project already isolates the
windowing system behind a backend vtable (`backend_internal.h`), and almost
everything else is portable POSIX or uses libraries available on Homebrew.

| Area | Status on macOS | Work needed |
|---|---|---|
| CLI, config, timer, util | Portable POSIX/C11 (`getopt_long`, `clock_gettime`, `flock` all exist) | None |
| IPC (`ipc.c`) | Portable, except `SOCK_CLOEXEC` (Linux-only flag) | Tiny fix |
| Daemonize (`fork`/`setsid`) | Works; fork happens before any Cocoa init, which is required | None |
| Render (Pango + Cairo) | Available via Homebrew | None |
| Audio (libao + libsndfile) | Available via Homebrew; libao has a native `macosx` driver | None |
| i18n (gettext) | Not in libc; provided by Homebrew `gettext` (`libintl`) | Link `Intl` in CMake |
| XDG paths | Fallbacks (`~/.config`, `~/.local/...`) already work | None |
| `XDG_RUNTIME_DIR` | Not set on macOS; current fallback `/tmp/overlay-timer-<uid>` works | Optional: prefer `$TMPDIR` |
| Window backend | X11/Wayland unusable | **New `backend_macos.m`** (~250 lines) |
| Event loop | Cocoa needs its events pumped on the main thread | Small change in `daemon.c` |

Not touched: `backend_x11.c`, `backend_wayland.c`, `render.c`, `timer.c`,
`audio.c`, `cli.c`, tests.

---

## 2. Design

### 2.1 New backend: `src/backend_macos.m` (Objective-C, AppKit)

Same model as the X11 backend: **one full-screen transparent window per
selected screen**, drawn with the existing `render_frame()`.

- **Init**
  - `[NSApplication sharedApplication]`,
    `setActivationPolicy:NSApplicationActivationPolicyAccessory`
    (no Dock icon, no menu bar, never steals focus).
  - Screens: `[NSScreen screens]`, indexed like RandR monitors
    (index 0 = main screen). Use `visibleFrame` so the text does not sit under
    the menu bar or the Dock.
- **Window per screen** (`NSWindow`, `NSWindowStyleMaskBorderless`):
  - `opaque = NO`, `backgroundColor = clearColor`, `hasShadow = NO`
  - `ignoresMouseEvents = YES` → **click-through**
  - `level = NSStatusWindowLevel` → stays above normal windows
  - `collectionBehavior = CanJoinAllSpaces | Stationary |
    FullScreenAuxiliary | IgnoresCycle` → visible on every Space, excluded
    from Cmd+Tab / Mission Control
  - `orderFrontRegardless` (shows without activating the app)
  - content view is layer-backed (`wantsLayer = YES`)
- **Render**
  - Create a `cairo_image_surface` (ARGB32) at `width × height × backingScaleFactor`
    (Retina), `cairo_scale()` by the factor, call `render_frame()` with the
    logical size.
  - Wrap the pixels in a `CGImage` (`CGDataProvider` +
    `kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little`, which
    matches Cairo's ARGB32 layout) and assign it to `view.layer.contents`.
  - `[CATransaction flush]` so the change is shown without running the
    Cocoa run loop.
- **Vtable**
  - `get_fd` → `-1` (Cocoa has no pollable fd).
  - `pump` → drain pending events without blocking:
    `nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
    inMode:NSDefaultRunLoopMode dequeue:YES` + `sendEvent:` in a loop.
  - `destroy` → close windows, release resources.
  - `name` → `"macos"`.

### 2.2 Event loop (`daemon.c`)

`poll()` already ignores negative fds, so passing `-1` is safe. Only change:
when the backend has no fd, call `backend_pump_events()` on **every** loop
iteration instead of only on `POLLIN`. The loop wakes at least every second
(and every 350 ms while blinking), which is enough for a window that takes no
input.

```c
if (backend_fd < 0 || (pfds[1].revents & POLLIN))
    backend_pump_events(backend);
```

No threads, no `NSApp run`, and the rest of the daemon stays unchanged.

### 2.3 Backend selection

- `config.h`: add `BACKEND_MACOS` to `backend_t`.
- `config.c`: accept/emit `"macos"` for the `backend` key.
- `backend.c`: when built with `WITH_MACOS`, `detect_backend()` returns
  `BACKEND_MACOS`; dispatch to `backend_create_macos()`.
- `backend_internal.h`: declare `backend_create_macos()` under `WITH_MACOS`.
- No new CLI flag (`auto` is enough on macOS; `backend = macos` in the INI
  also works).

### 2.4 Small portability fixes

- `ipc.c`: replace `socket(..., SOCK_STREAM | SOCK_CLOEXEC, 0)` with
  `socket(..., SOCK_STREAM, 0)` followed by
  `fcntl(fd, F_SETFD, FD_CLOEXEC)` (works on both Linux and macOS; 2 places).
- `util.c` (optional): in `xdg_runtime_dir()`, fall back to `$TMPDIR`
  (per-user, private on macOS) before `/tmp/overlay-timer-<uid>`. Keep the
  socket path under macOS' 104-byte `sun_path` limit (already checked in
  `ipc.c`).

### 2.5 Build (`CMakeLists.txt`)

Implemented with two differences from this sketch: the backend is compiled
**without ARC** (so `NSWindow *` can live in plain C structs), and the
pkg-config dependencies use `IMPORTED_TARGET` so Homebrew library paths are
linked correctly.

```cmake
option(WITH_MACOS "Build the macOS (AppKit) backend" ${APPLE})
# On APPLE, default WITH_X11 / WITH_WAYLAND to OFF.
# The "at least one backend" check includes WITH_MACOS.

if(WITH_MACOS)
    enable_language(OBJC)
    target_sources(overlay_timer_core PRIVATE src/backend_macos.m)
    set_source_files_properties(src/backend_macos.m PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
    target_compile_definitions(overlay_timer_core PUBLIC WITH_MACOS)
    target_link_libraries(overlay_timer_core PUBLIC
        "-framework AppKit" "-framework QuartzCore")
endif()

find_package(Intl REQUIRED)   # libintl on macOS; no-op on glibc
target_include_directories(overlay_timer_core PUBLIC ${Intl_INCLUDE_DIRS})
target_link_libraries(overlay_timer_core PUBLIC ${Intl_LIBRARIES})
```

- `add_definitions(-D_GNU_SOURCE)` is harmless on macOS; keep it.
- Install layout (`GNUInstallDirs`, man page, `.mo` files) works unchanged
  under the Homebrew prefix.

### 2.6 Dependencies (Homebrew)

```sh
brew install cmake pkg-config cairo pango libao libsndfile gettext
```

---

## 3. Steps

1. **Portability fixes**: `SOCK_CLOEXEC` → `fcntl`; `Intl` in CMake;
   `WITH_MACOS` option and defaults. Verify that it still builds and all
   tests pass on Linux.
2. **Backend selection**: `BACKEND_MACOS` enum, config string, autodetection,
   constructor declaration.
3. **`backend_macos.m`**: windows per screen, click-through, Cairo → CGImage
   rendering with Retina scale, non-blocking event pump.
4. **Daemon loop**: pump events every iteration when `get_fd() == -1`.
5. **Validate on a Mac**: build, `ctest`, then manually check countdown,
   pomodoro, the 4 corners, multi-screen, pause/resume/stop/move via IPC,
   sound, blink, `--help` in pt_BR, and that the overlay stays visible over
   full-screen apps and across Spaces.
6. **Docs**: README (macOS build + Homebrew deps, backend section), man page
   (`macos` value for `backend`, `$TMPDIR` note), PLAN.md progress entry;
   update `po/` if new user-visible strings were added.

---

## 4. Known limitations / risks

- **Fork + Cocoa**: AppKit must only be initialized **after** `daemonize()`.
  This is already the case (the backend is created inside `daemon_run()`);
  keep it that way.
- **No Dock icon / not a `.app` bundle**: intentional. A bare executable can
  still create windows as an accessory app. Notarization/signing is out of
  scope.
- **Screen changes** (monitor plugged/unplugged) are not handled, same as the
  current X11 backend.
- **Event latency**: Cocoa events are processed at the timer cadence
  (≤ 1 s). Acceptable because the overlay ignores all input.
- **Fonts**: Homebrew Pango uses CoreText/fontconfig; font family names may
  differ from Linux (e.g. `Menlo`, `SF Mono`).
