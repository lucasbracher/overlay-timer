# Live Screen Changes & Monitor Hot-plug Plan — overlay-timer

> **Status:** implemented (steps 1–7, including per-monitor DPI on Windows).
> Tested on X11 (live `--screen` changes; hot-plug simulated with a RandR
> virtual monitor + `xrandr --fb` to emit real RandR events). The Wayland
> backend builds on Linux but was not run (X11 session). The Windows and macOS
> backends were **not compiled** (no MinGW/macOS SDK on the dev machine) and
> still need a build and the test matrix of section 6 on real hardware.
>
> Implementation notes vs. the plan: the last-frame cache lives in the
> generic wrapper (`backend.c`), and the debounced rebuild runs from a new
> `tick()` vtable entry that the daemon calls every loop iteration (X11 and
> Wayland are only pumped when their fd is readable). On Wayland `wl_output`
> is still bound at v2 (no `name` event), to keep building against older
> wayland-client headers.

## 1. Problem

1. **`--screen` cannot be changed while the timer runs.** The CLI already sends
   `SET screen=0,1` and `handle_set()` (`daemon.c`) already updates
   `st.cfg.screens`, but every backend builds its overlay windows **once**, in
   `backend_create_*()`, from `cfg->screens`. Nothing ever rebuilds them, so the
   daemon answers `OK 1 applied` and the screen does not change.
2. **Monitor connect/disconnect is not detected.** No backend listens for
   topology changes. When a monitor goes away, the OS moves the windows that
   were on it (Windows and macOS do this; X11/Wayland compositors behave in
   different ways), so two overlays end up **stacked on the same screen**. When
   the monitor comes back, nothing is recreated on it.

Both problems have the same fix: **each backend must be able to throw away its
overlays and rebuild them from a fresh list of monitors**, both when asked by
the daemon (`--screen`) and when the backend detects a topology change.

## 2. Semantics (decisions)

- `cfg.screens` is the **requested** set (what the user asked for). It does not
  change on hot-plug.
- The **effective** set is computed on every (re)build:
  `requested ∩ [0, n_monitors)`, with duplicates removed.
- **Fallback:** if the effective set is empty but at least one monitor exists,
  show on screen `0` (primary), so the timer never vanishes silently. If no
  monitor exists (e.g. laptop lid closed + external unplugged), keep zero
  overlays and wait; **do not exit**.
- When the missing monitor comes back, the next rebuild puts the overlay on it
  again (because the requested set never changed).
- Indices keep their current meaning (enumeration order, 0 = primary where the
  platform has one). Identifying monitors by name/ID (RandR monitor name,
  `wl_output.name`, `\\.\DISPLAYn`, `CGDirectDisplayID`) would be more stable
  across hot-plug, but it is **out of scope** — noted as a possible follow-up.
- `--screen` while running: the reply reports what was actually applied, e.g.
  `OK screens: 0,1` or `OK screens: 0 (screen 2 not available, have 2)`.
  It's still `OK` (the request was stored and will be honoured when the
  monitor appears).

## 3. Common design

### 3.1 Backend interface (`backend.h`, `backend_internal.h`, `backend.c`)

New public function and vtable entry:

```c
/* Replace the requested screen list and rebuild the overlays now.
 * Returns the number of overlays shown; *msg (optional, caller frees)
 * receives a human-readable summary/warning for the IPC reply. */
int backend_set_screens(overlay_backend_t *b, const int *screens, int n,
                        char **msg);

/* vtable */
int (*set_screens)(void *impl, const int *screens, int n, char **msg);
```

Shared helpers in `backend.c` (pure, no window system → unit-testable):

```c
/* requested ∩ [0,nmon), deduplicated, with fallback to 0.
 * Returns the count written to `out`. */
int backend_resolve_screens(const int *req, int nreq, int nmon,
                            int *out, int max);
```

And a small shared "last frame" cache (`frame_cache_t` with `store()` and the
owned `text`/`label` copies) in `backend_internal.h`, replacing the duplicated
`store_last()` in X11/Wayland and **adding** it to Win32/macOS, which currently
don't keep the last frame. After any rebuild the backend redraws the cached
frame immediately, so the new overlay doesn't sit blank until the next tick.

### 3.2 Per-backend refactor (same shape on all four)

Each backend's state gains:

```c
int  requested[MAX_SCREENS]; int n_requested;
bool topology_dirty;        /* set by event handlers */
long dirty_since_ms;        /* debounce */
```

and its creation loop is split into:

- `destroy_overlays(b)` — frees windows/surfaces only, keeps the connection.
- `build_overlays(b)` — enumerates monitors, calls `backend_resolve_screens()`,
  creates one overlay per effective screen, redraws the cached frame, logs
  `screens: N monitor(s), showing on 0,1`.

`backend_create_*()` = connect + `build_overlays()`.
`set_screens()` = copy `requested` + `destroy_overlays()` + `build_overlays()`.
`pump()` = drain events; if `topology_dirty` and ≥ 300 ms have passed since the
last change event, rebuild once. Monitor changes arrive in **bursts** (several
RandR/CG/Win32 notifications per plug), so the debounce avoids rebuilding 3–5
times. The daemon loop already wakes at least once per second, which is enough
to service the debounce; no daemon timer changes needed.

`backend_create_*()` keeps failing with "no valid screen" only when there are
zero monitors **at startup**; after that, zero overlays is a valid state.

### 3.3 Daemon / IPC (`daemon.c`)

- Keep the `overlay_backend_t *` in `daemon_state_t` so `handle_command()` can
  reach it.
- In `handle_set()`, remember whether a `screen` key was applied; if so, call
  `backend_set_screens()` and append its message to the reply.
- Replace the `fprintf(stderr, "warning: screen index %d out of range")` calls
  in the backends with `log_warn()` — after `daemonize()` stderr is
  `/dev/null`, so today those warnings are lost.
- `backend=` via `SET` cannot be honoured live: reply `ERR backend cannot be
  changed while running` instead of silently storing it (small, related fix).
- Optional: `STATUS` also reports `screens=0,1`.

No changes to `cli.c`, `config.c` or the IPC wire protocol: `screen=` is
already serialised and parsed.

## 4. Per-platform details

### 4.1 X11 (`backend_x11.c`)

- **Detection:** at creation,
  `xcb_randr_select_input(conn, root, SCREEN_CHANGE | OUTPUT_CHANGE | CRTC_CHANGE)`
  and read `xcb_get_extension_data(conn, &xcb_randr_id)->first_event`.
  In `x11_pump()`, events of type `first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY`
  or `first_event + XCB_RANDR_NOTIFY` set `topology_dirty`.
- **Rebuild:** destroy cairo surfaces + `xcb_destroy_window`, then
  `enumerate_monitors()` again (RandR 1.5 `get_monitors`, already used).
  Note: `b->screen->width_in_pixels` comes from the connection setup and is
  stale after a change; the no-RandR fallback should query the root geometry
  (`xcb_get_geometry`) instead.
- `overlays` is a heap array sized by `n_screens`; reallocate on rebuild (or
  make it a fixed `MAX_SCREENS` array like Wayland).
- Why they stack today: override-redirect windows aren't managed by the WM, so
  they keep their old root coordinates; after a layout shift those
  coordinates fall on another monitor (or off-screen).

### 4.2 Wayland (`backend_wayland.c`)

- **Outputs become dynamic.** Store the registry `name` in `wl_output_info_t`.
  - `registry_global(wl_output)` after startup: bind it and add it; mark dirty
    **only on its `done` event** (mode/scale are not known before that).
    `output_done` currently does nothing; it will set `have_done` + dirty.
  - `registry_global_remove(name)`: find the matching output, destroy it,
    compact the array, mark dirty. (Today it is a no-op.)
- **`layer_surface.closed`:** the compositor sends it when the output goes
  away. Per protocol the client must destroy the surface; today `ls_closed`
  only clears `configured`. It will mark dirty, and the rebuild destroys it.
- **Rebuild:** destroy layer surfaces, surfaces and SHM buffers of all
  overlays; create new ones and commit without buffer. **No roundtrip in
  pump** — `ls_configure` already draws the cached frame when the configure
  arrives, so the normal event flow is enough.
- Bind `wl_output` at version up to 4 to get the `name` event (useful for logs
  now, for name-based selection later); must stay compatible with v2.
- Indices follow registry advertisement order, so a reconnected output goes
  to the **end** of the list. Document this.

### 4.3 Windows (`backend_win32.c`)

- **Detection:** the class uses `DefWindowProcW` today; replace it with a real
  `WndProc` that sets `topology_dirty` on:
  - `WM_DISPLAYCHANGE` (monitor added/removed, resolution change);
  - `WM_SETTINGCHANGE` with `wParam == SPI_SETWORKAREA` (taskbar moved/resized —
    the overlay covers the work area);
  - `WM_DPICHANGED` (see below).
  The backend pointer is passed via `CreateWindowExW`'s `lpParam` →
  `GWLP_USERDATA`.
- **Hidden helper window:** these are broadcast to **top-level** windows only
  (message-only `HWND_MESSAGE` windows do not get them). When the effective
  set is empty there may be no overlay window to receive them, so create one
  hidden, never-shown top-level window at startup that lives for the whole
  backend lifetime.
- **Rebuild:** `DestroyWindow` + destroy the DIB surfaces, re-run
  `EnumDisplayMonitors` (primary first, as today), recreate.
  The window class is registered once and stays registered until
  `win_destroy()`.
- Why they stack today: Windows moves windows from a removed monitor to the
  primary one.
- **DPI (optional, recommended):** today `SetProcessDPIAware()` is system-wide
  DPI. With monitors of different scale factors, coordinates of secondary
  monitors may be virtualised. Switching to
  `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`
  (fallback to the old call on older Windows) makes `rcWork` real pixels on
  every monitor. Can be a separate step.

### 4.4 macOS (`backend_macos.m`)

- **Detection:** `CGDisplayRegisterReconfigurationCallback()` (C callback;
  ignore calls with `kCGDisplayBeginConfigurationFlag`, mark dirty on the
  others) **and/or** observe `NSApplicationDidChangeScreenParametersNotification`
  (also covers Dock/menu-bar changes to `visibleFrame`).
  Unregister in `mac_destroy()`.
- **Risk:** the NSApp run loop is never started; we only drain events with
  `nextEventMatchingMask:` in `NSDefaultRunLoopMode`. That call runs the run
  loop briefly, which should deliver both kinds of notification, but this
  must be verified on real hardware. **Fallback** if it doesn't: in `pump()`,
  once per second compare a cheap signature of `[NSScreen screens]` (count +
  frames + displayIDs) with the last one and mark dirty when it changes.
- **Rebuild:** `orderOut` / `close` / `release` the windows, free surfaces,
  re-read `[NSScreen screens]` (index 0 = screen with the menu bar), recreate.
  Wrap in `@autoreleasepool`.
- Why they stack today: macOS moves windows from a removed screen to a
  remaining one.

## 5. Steps

1. **Common:** `backend_resolve_screens()`, `frame_cache_t`, `set_screens`
   vtable entry + `backend_set_screens()`; unit tests in
   `tests/test_backend_common.c` (dedup, out of range, fallback, zero
   monitors). No window system needed.
2. **Daemon:** backend pointer in state, `screen=` → `backend_set_screens()`,
   reply text, `backend=` rejected live, `log_warn` instead of `stderr`.
3. **X11:** split create loop, RandR events, debounce, rebuild.
4. **Wayland:** dynamic outputs, `global_remove`, `closed`, rebuild.
5. **Windows:** `WndProc`, helper window, rebuild (DPI as optional sub-step).
6. **macOS:** reconfiguration callback / notification, rebuild, fallback
   polling if needed.
7. **Docs & i18n:** README + man page (`--screen` works live; hot-plug
   behaviour; index meaning after reconnect), regenerate `po/overlay-timer.pot`
   and update `pt_BR.po` / `en.po` with the new messages.

Each backend step is independent after steps 1–2, so they can be done and
tested one OS at a time.

## 6. Manual test matrix

For each OS, start with `--pomodoro 4 --screen 0,1` on two monitors, then:

| Scenario | X11 | Wayland (sway/Hyprland/KDE) | Windows | macOS |
|---|---|---|---|---|
| Live `--screen 1`, `--screen 0,1`, `--screen 0` | — | — | — | — |
| Invalid `--screen 5` → fallback + message | — | — | — | — |
| Disconnect monitor 1 → one overlay only, no stacking | `xrandr --output X --off` | `swaymsg output X disable` / unplug | Win+P "PC screen only" / unplug | unplug / mirror |
| Reconnect → overlay comes back on it | `xrandr --output X --auto` | `... enable` | Win+P "Extend" | replug |
| Change resolution / move taskbar / Dock | `xrandr --mode` | output mode | taskbar position | Dock position |
| Disconnect **all** external + lid closed → daemon survives; reopen → overlay back | ✓ | ✓ | ✓ | ✓ |
| Change happens during the end-of-timer blink → no crash | ✓ | ✓ | ✓ | ✓ |

## 7. Out of scope / follow-ups

- Selecting monitors by name/ID (`--screen HDMI-1`).
- Per-monitor size/corner.
- Changing backend (`--x11`/`--wayland`) while running.
