#include "daemon.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backend.h"
#include "config.h"
#include "i18n.h"
#include "ipc.h"
#include "timer.h"
#include "util.h"
#include "audio.h"

typedef struct {
    config_t cfg;
    otimer_t  timer;
    audio_t  *audio;
    volatile sig_atomic_t stop_requested;
} daemon_state_t;

static daemon_state_t *g_state = NULL;
static volatile sig_atomic_t g_signal = 0;

static void on_signal(int sig) {
    g_signal = sig;
    if (g_state) g_state->stop_requested = 1;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Cor conforme a fase atual. */
static void pick_color(daemon_state_t *st, rgb_t *out) {
    const char *name;
    if (st->timer.kind == TIMER_POMODORO)
        name = timer_in_break(&st->timer) ? st->cfg.break_color
                                          : st->cfg.pomodoro_color;
    else
        name = st->cfg.color;
    if (!parse_color(name, out)) { out->r = 255; out->g = 0; out->b = 0; }
}

static void build_frame(daemon_state_t *st, overlay_frame_t *f, bool visible) {
    static char textbuf[16];
    static char labelbuf[64];
    timer_text(&st->timer, textbuf, sizeof textbuf);
    timer_label(&st->timer, labelbuf, sizeof labelbuf);

    rgb_t c;
    pick_color(st, &c);

    f->text = textbuf;
    f->label = labelbuf[0] ? labelbuf : NULL;
    f->color = c;
    f->alpha = st->cfg.alpha;
    f->size = st->cfg.size;
    f->font = st->cfg.font;
    f->corner = st->cfg.corner;
    f->visible = visible;
}

static void handle_set(daemon_state_t *st, char *args, char *resp, size_t rsz) {
    int applied = 0, failed = 0;
    char *save = NULL;
    for (char *tok = strtok_r(args, " \t", &save); tok;
         tok = strtok_r(NULL, " \t", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) { failed++; continue; }
        *eq = '\0';
        if (config_set_kv(&st->cfg, tok, eq + 1)) applied++;
        else failed++;
    }
    if (failed) snprintf(resp, rsz, "ERR %d applied, %d invalid", applied, failed);
    else        snprintf(resp, rsz, "OK %d applied", applied);
    log_info(_("reconfigure: %d applied, %d invalid"), applied, failed);
}

static bool handle_command(daemon_state_t *st, char *line, char *resp, size_t rsz) {
    resp[0] = '\0';

    char *sp = line;
    while (*sp && *sp != ' ') sp++;
    char verb[32];
    size_t vlen = (size_t)(sp - line);
    if (vlen >= sizeof verb) vlen = sizeof verb - 1;
    memcpy(verb, line, vlen);
    verb[vlen] = '\0';
    while (*sp == ' ') sp++;
    char *args = sp;

    if (str_ieq(verb, "PAUSE"))  { timer_pause(&st->timer);  snprintf(resp, rsz, "OK paused");  log_info(_("paused"));  return false; }
    if (str_ieq(verb, "RESUME")) { timer_resume(&st->timer); snprintf(resp, rsz, "OK resumed"); log_info(_("resumed")); return false; }
    if (str_ieq(verb, "STOP"))   { snprintf(resp, rsz, "OK stopping"); log_info(_("stop requested via IPC")); return true; }
    if (str_ieq(verb, "STATUS")) {
        char buf[16];
        timer_text(&st->timer, buf, sizeof buf);
        snprintf(resp, rsz, "OK %s %s", buf,
                 timer_is_paused(&st->timer) ? "paused" : "running");
        return false;
    }
    if (str_ieq(verb, "SET")) { handle_set(st, args, resp, rsz); return false; }

    snprintf(resp, rsz, "ERR unknown command: %s", verb);
    return false;
}

#define BLINK_PERIOD_MS 350
#define BLINK_DURATION_MS 4000

int daemon_run(const cli_opts_t *opts) {
    daemon_state_t st;
    memset(&st, 0, sizeof st);
    config_copy(&st.cfg, &opts->cfg);
    g_state = &st;

    if (opts->action == ACTION_START_POMODORO) {
        timer_init_pomodoro(&st.timer, opts->pomodoro_count, &st.cfg);
        log_info(_("daemon: starting %d pomodoro(s)"), opts->pomodoro_count);
    } else {
        timer_init_countdown(&st.timer, opts->time_seconds, st.cfg.format);
        log_info(_("daemon: starting countdown of %ld s"), opts->time_seconds);
    }

    ipc_server_t srv;
    if (ipc_server_start(&srv) != 0) {
        log_error(_("daemon: could not start IPC server"));
        config_free(&st.cfg);
        return 1;
    }

    char *berr = NULL;
    overlay_backend_t *backend = backend_create(st.cfg.backend, &st.cfg, &berr);
    if (!backend) {
        log_error(_("daemon: backend init failed: %s"), berr ? berr : "?");
        free(berr);
        ipc_server_stop(&srv);
        config_free(&st.cfg);
        return 1;
    }
    log_info(_("daemon: backend '%s' ready (%d screen(s))"),
             backend_name(backend), st.cfg.n_screens);

    st.audio = audio_init(st.cfg.sounds_dir, st.cfg.enable_sound);

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    overlay_frame_t frame;
    build_frame(&st, &frame, true);
    backend_render(backend, &frame);

    int backend_fd = backend_get_fd(backend);
    long next_tick = now_ms() + 1000;

    bool finished = false;
    bool blink_on = true;
    long blink_toggle_at = 0;
    long blink_end_at = 0;
    int exit_code = 0;

    while (!st.stop_requested) {
        long now = now_ms();
        long timeout = next_tick - now;
        if (finished) {
            long bt = blink_toggle_at - now;
            if (bt < timeout) timeout = bt;
        }
        if (timeout < 0) timeout = 0;

        struct pollfd pfds[2];
        pfds[0].fd = srv.listen_fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = backend_fd;    pfds[1].events = POLLIN; pfds[1].revents = 0;

        int pr = poll(pfds, 2, (int)timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            log_error(_("poll failed: %s"), strerror(errno));
            exit_code = 1;
            break;
        }

        if (pfds[1].revents & POLLIN)
            backend_pump_events(backend);

        if (pfds[0].revents & POLLIN) {
            char line[1024], resp[1024];
            int cfd = ipc_server_accept(&srv, line, sizeof line);
            if (cfd >= 0) {
                bool should_stop = handle_command(&st, line, resp, sizeof resp);
                ipc_server_reply(cfd, resp);
                if (!finished) {
                    build_frame(&st, &frame, true);
                    backend_render(backend, &frame);
                }
                if (should_stop) break;
            }
        }

        now = now_ms();

        if (!finished && now >= next_tick) {
            next_tick += 1000;
            timer_event_t ev = timer_tick(&st.timer);

            build_frame(&st, &frame, true);
            backend_render(backend, &frame);

            switch (ev) {
            case TEVENT_POMODORO_END:
                log_info(_("pomodoro ended -> break"));
                audio_play(st.audio, SOUND_POMODORO_END);
                break;
            case TEVENT_BREAK_END:
                log_info(_("break ended -> pomodoro"));
                audio_play(st.audio, SOUND_BREAK_END);
                break;
            case TEVENT_FINISHED:
                log_info(_("timer finished"));
                audio_play(st.audio, SOUND_TIMER_END);
                finished = true;
                blink_on = false;
                blink_toggle_at = now + BLINK_PERIOD_MS;
                blink_end_at = now + BLINK_DURATION_MS;
                break;
            case TEVENT_NONE:
            default:
                break;
            }
        }

        if (finished) {
            if (now >= blink_end_at) break;
            if (now >= blink_toggle_at) {
                blink_toggle_at = now + BLINK_PERIOD_MS;
                blink_on = !blink_on;
                build_frame(&st, &frame, blink_on);
                backend_render(backend, &frame);
            }
        }
    }

    if (g_signal)
        log_info(_("daemon: received signal %d, shutting down"), (int)g_signal);

    audio_shutdown(st.audio);
    backend_destroy(backend);
    ipc_server_stop(&srv);
    config_free(&st.cfg);
    g_state = NULL;
    return exit_code;
}
