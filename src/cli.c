#include "cli.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"

#ifndef OVERLAY_TIMER_VERSION
#define OVERLAY_TIMER_VERSION "0.1.0"
#endif

/* Identifiers for long options without a short form. */
enum {
    OPT_ALPHA = 1000,
    OPT_SIZE,
    OPT_FONT,
    OPT_CORNER,
    OPT_SCREEN,
    OPT_FORMAT,
    OPT_POMODORO,
    OPT_POMODORO_MINUTES,
    OPT_BREAK_MINUTES,
    OPT_LONG_BREAK_MINUTES,
    OPT_LONG_BREAK_EVERY,
    OPT_POMODORO_COLOR,
    OPT_BREAK_COLOR,
    OPT_PAUSE,
    OPT_RESUME,
    OPT_STOP,
    OPT_X11,
    OPT_WAYLAND,
    OPT_CONFIG,
    OPT_SAVE,
    OPT_SOUNDS_DIR,
    OPT_NO_SOUND,
    OPT_SOUND
};

static const struct option LONG_OPTS[] = {
    {"time",               required_argument, 0, 't'},
    {"color",              required_argument, 0, 'c'},
    {"alpha",              required_argument, 0, OPT_ALPHA},
    {"size",               required_argument, 0, OPT_SIZE},
    {"font",               required_argument, 0, OPT_FONT},
    {"corner",             required_argument, 0, OPT_CORNER},
    {"screen",             required_argument, 0, OPT_SCREEN},
    {"format",             required_argument, 0, OPT_FORMAT},
    {"pomodoro",           required_argument, 0, OPT_POMODORO},
    {"pomodoro-minutes",   required_argument, 0, OPT_POMODORO_MINUTES},
    {"break-minutes",      required_argument, 0, OPT_BREAK_MINUTES},
    {"long-break-minutes", required_argument, 0, OPT_LONG_BREAK_MINUTES},
    {"long-break-every",   required_argument, 0, OPT_LONG_BREAK_EVERY},
    {"pomodoro-color",     required_argument, 0, OPT_POMODORO_COLOR},
    {"break-color",        required_argument, 0, OPT_BREAK_COLOR},
    {"pause",              no_argument,       0, OPT_PAUSE},
    {"resume",             no_argument,       0, OPT_RESUME},
    {"stop",               no_argument,       0, OPT_STOP},
    {"x11",                no_argument,       0, OPT_X11},
    {"wayland",            no_argument,       0, OPT_WAYLAND},
    {"sounds-dir",         required_argument, 0, OPT_SOUNDS_DIR},
    {"no-sound",           no_argument,       0, OPT_NO_SOUND},
    {"sound",              no_argument,       0, OPT_SOUND},
    {"config",             required_argument, 0, OPT_CONFIG},
    {"save",               no_argument,       0, OPT_SAVE},
    {"help",               no_argument,       0, 'h'},
    {"version",            no_argument,       0, 'V'},
    {0, 0, 0, 0}
};

void cli_print_version(void) {
    printf("overlay-timer %s\n", OVERLAY_TIMER_VERSION);
}

void cli_print_help(const char *prog) {
    printf(_("Usage: %s [options]\n"), prog);
    printf(_(
"\n"
"Translucent, click-through countdown/pomodoro overlay timer.\n"
"\n"
"Time / mode:\n"
"  -t, --time <MM:SS|HH:MM:SS>  start a countdown\n"
"      --pomodoro <N>           start N pomodoros\n"
"      --format <hhmmss|mmss>   display format (actual time takes precedence)\n"
"\n"
"Appearance:\n"
"  -c, --color <color>          text color (name or #RRGGBB)\n"
"      --alpha <0-100>          visibility (0=transparent, 100=opaque)\n"
"      --size <0-100>           text size as %% of the screen's smaller side\n"
"      --font <family>          font family\n"
"      --corner <tl|tr|bl|br>   corner\n"
"      --screen <idx[,idx...]>  screen index(es)\n"
"\n"
"Pomodoro:\n"
"      --pomodoro-minutes <m>\n"
"      --break-minutes <m>\n"
"      --long-break-minutes <m>\n"
"      --long-break-every <N>   0 = no long break (default)\n"
"      --pomodoro-color <color>\n"
"      --break-color <color>\n"
"\n"
"Control (requires a running timer):\n"
"      --pause | --resume | --stop\n"
"\n"
"Audio:\n"
"      --sounds-dir <path>      directory with .wav files\n"
"      --sound | --no-sound     enable/disable sound\n"
"\n"
"Window system:\n"
"      --x11 | --wayland\n"
"\n"
"Config:\n"
"      --config <path>          use an alternate config file\n"
"      --save                   merge current options into the config file\n"
"\n"
"General:\n"
"  -h, --help\n"
"  -V, --version\n"));
}

bool cli_parse(int argc, char **argv, cli_opts_t *opts) {
    opts->action = ACTION_NONE;
    opts->time_seconds = 0;
    opts->pomodoro_count = 0;
    opts->config_path = NULL;
    opts->save = false;
    memset(&opts->set, 0, sizeof opts->set);

    config_t *cfg = &opts->cfg;
    bool control_action = false; /* pause/resume/stop */

    /* Allow re-entrancy in tests. */
    optind = 1;

    int c;
    while ((c = getopt_long(argc, argv, "t:c:hV", LONG_OPTS, NULL)) != -1) {
        switch (c) {
        case 't': {
            long secs;
            if (!parse_time(optarg, &secs)) {
                fprintf(stderr, _("error: invalid time: '%s'\n"), optarg);
                return false;
            }
            opts->time_seconds = secs;
            opts->action = ACTION_START_TIMER;
            break;
        }
        case OPT_POMODORO: {
            char *end;
            long n = strtol(optarg, &end, 10);
            if (*end != '\0' || n <= 0 || n > 1000) {
                fprintf(stderr, _("error: invalid pomodoro count: '%s'\n"), optarg);
                return false;
            }
            opts->pomodoro_count = (int)n;
            opts->action = ACTION_START_POMODORO;
            break;
        }
        case 'c':
            free(cfg->color); cfg->color = xstrdup(optarg);
            opts->set.color = true;
            break;
        case OPT_ALPHA: {
            char *end; long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v > 100) { fprintf(stderr, _("error: alpha must be 0-100\n")); return false; }
            cfg->alpha = (int)v; opts->set.alpha = true; break;
        }
        case OPT_SIZE: {
            char *end; long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v > 100) { fprintf(stderr, _("error: size must be 0-100\n")); return false; }
            cfg->size = (int)v; opts->set.size = true; break;
        }
        case OPT_FONT:
            free(cfg->font); cfg->font = xstrdup(optarg);
            opts->set.font = true; break;
        case OPT_CORNER:
            if (!corner_from_str(optarg, &cfg->corner)) {
                fprintf(stderr, _("error: corner must be tl|tr|bl|br\n")); return false;
            }
            opts->set.corner = true; break;
        case OPT_SCREEN: {
            int tmp[MAX_SCREENS], n;
            if (!parse_screen_list(optarg, tmp, MAX_SCREENS, &n)) {
                fprintf(stderr, _("error: invalid screen list: '%s'\n"), optarg); return false;
            }
            memcpy(cfg->screens, tmp, sizeof(int) * n); cfg->n_screens = n;
            opts->set.screen = true; break;
        }
        case OPT_FORMAT:
            if (!format_from_str(optarg, &cfg->format)) {
                fprintf(stderr, _("error: format must be hhmmss|mmss\n")); return false;
            }
            opts->set.format = true; break;
        case OPT_POMODORO_MINUTES: {
            char *end; long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 1440) { fprintf(stderr, _("error: invalid pomodoro-minutes\n")); return false; }
            cfg->pomodoro_minutes = (int)v; opts->set.pomodoro_minutes = true; break;
        }
        case OPT_BREAK_MINUTES: {
            char *end; long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v > 1440) { fprintf(stderr, _("error: invalid break-minutes\n")); return false; }
            cfg->break_minutes = (int)v; opts->set.break_minutes = true; break;
        }
        case OPT_LONG_BREAK_MINUTES: {
            char *end; long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v > 1440) { fprintf(stderr, _("error: invalid long-break-minutes\n")); return false; }
            cfg->long_break_minutes = (int)v; opts->set.long_break_minutes = true; break;
        }
        case OPT_LONG_BREAK_EVERY: {
            char *end; long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v > 100) { fprintf(stderr, _("error: invalid long-break-every\n")); return false; }
            cfg->long_break_every = (int)v; opts->set.long_break_every = true; break;
        }
        case OPT_POMODORO_COLOR:
            free(cfg->pomodoro_color); cfg->pomodoro_color = xstrdup(optarg);
            opts->set.pomodoro_color = true; break;
        case OPT_BREAK_COLOR:
            free(cfg->break_color); cfg->break_color = xstrdup(optarg);
            opts->set.break_color = true; break;
        case OPT_SOUNDS_DIR:
            free(cfg->sounds_dir); cfg->sounds_dir = xstrdup(optarg);
            opts->set.sounds_dir = true; break;
        case OPT_NO_SOUND:
            cfg->enable_sound = false; opts->set.enable_sound = true; break;
        case OPT_SOUND:
            cfg->enable_sound = true; opts->set.enable_sound = true; break;
        case OPT_PAUSE:
            opts->action = ACTION_PAUSE; control_action = true; break;
        case OPT_RESUME:
            opts->action = ACTION_RESUME; control_action = true; break;
        case OPT_STOP:
            opts->action = ACTION_STOP; control_action = true; break;
        case OPT_X11:
            cfg->backend = BACKEND_X11; opts->set.backend = true; break;
        case OPT_WAYLAND:
            cfg->backend = BACKEND_WAYLAND; opts->set.backend = true; break;
        case OPT_CONFIG:
            free(opts->config_path); opts->config_path = xstrdup(optarg); break;
        case OPT_SAVE:
            opts->save = true; break;
        case 'h':
            opts->action = ACTION_HELP; return true;
        case 'V':
            opts->action = ACTION_VERSION; return true;
        case '?':
        default:
            return false; /* getopt already printed the message */
        }
    }

    if (optind < argc) {
        fprintf(stderr, _("error: unexpected argument: '%s'\n"), argv[optind]);
        return false;
    }

    /* Resolve action conflicts. */
    if (control_action &&
        (opts->action == ACTION_START_TIMER ||
         opts->action == ACTION_START_POMODORO)) {
        /* control_action overrode action; however, if the user passed -t and
         * --stop together, we treat it as ambiguous. */
    }

    /* If no primary action was given but there are overrides, it is a reconfigure. */
    if (opts->action == ACTION_NONE) {
        static const typeof(opts->set) ZERO_SET = {0};
        if (memcmp(&opts->set, &ZERO_SET, sizeof opts->set) != 0)
            opts->action = ACTION_RECONFIGURE;
    }

    return true;
}

void cli_free(cli_opts_t *opts) {
    if (!opts) return;
    free(opts->config_path);
    opts->config_path = NULL;
    config_free(&opts->cfg);
}

int cli_build_set_message(const cli_opts_t *opts, char *buf, size_t bufsz) {
    const config_t *c = &opts->cfg;
    int n = 0;
    size_t off = 0;

    off += (size_t)snprintf(buf + off, bufsz - off, "SET");

#define APPEND(cond, fmt, ...) do { \
    if ((cond) && off < bufsz) { \
        off += (size_t)snprintf(buf + off, bufsz - off, " " fmt, __VA_ARGS__); \
        n++; \
    } \
} while (0)

    APPEND(opts->set.color, "color=%s", c->color);
    APPEND(opts->set.alpha, "alpha=%d", c->alpha);
    APPEND(opts->set.size, "size=%d", c->size);
    APPEND(opts->set.font, "font=%s", c->font);
    APPEND(opts->set.corner, "corner=%s", corner_to_str(c->corner));
    APPEND(opts->set.format, "format=%s", format_to_str(c->format));
    APPEND(opts->set.pomodoro_color, "pomodoro_color=%s", c->pomodoro_color);
    APPEND(opts->set.break_color, "break_color=%s", c->break_color);
    APPEND(opts->set.sounds_dir, "sounds_dir=%s", c->sounds_dir);
    APPEND(opts->set.enable_sound, "enable_sound=%s", c->enable_sound ? "true" : "false");
    APPEND(opts->set.backend, "backend=%s", backend_to_str(c->backend));

    if (opts->set.screen && off < bufsz) {
        off += (size_t)snprintf(buf + off, bufsz - off, " screen=");
        for (int i = 0; i < c->n_screens && off < bufsz; i++)
            off += (size_t)snprintf(buf + off, bufsz - off, "%s%d",
                                    i ? "," : "", c->screens[i]);
        n++;
    }

#undef APPEND
    return n;
}
