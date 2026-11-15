#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "config.h"
#include "daemon.h"
#include "i18n.h"
#include "ipc.h"
#include "util.h"

#ifndef OVERLAY_TIMER_LOCALEDIR
#define OVERLAY_TIMER_LOCALEDIR "/usr/share/locale"
#endif

static void i18n_init(void) {
    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, OVERLAY_TIMER_LOCALEDIR);
    textdomain(GETTEXT_PACKAGE);
}

/* Prepara o arquivo de log em $XDG_STATE_HOME/overlay-timer/log. */
static char *setup_log_path(void) {
    char *state = xdg_state_home();
    const char *rel = "/overlay-timer";
    size_t n = strlen(state) + strlen(rel) + 1;
    char *dir = xmalloc(n);
    snprintf(dir, n, "%s%s", state, rel);
    mkdirs(dir, 0755);

    size_t m = strlen(dir) + strlen("/log") + 1;
    char *logp = xmalloc(m);
    snprintf(logp, m, "%s/log", dir);
    free(state);
    free(dir);
    return logp;
}

/* Envia uma mensagem simples de controle e imprime a resposta. */
static int send_control(const char *msg) {
    char resp[1024];
    bool ok = ipc_client_request(msg, resp, sizeof resp);
    if (ok) {
        printf("%s\n", resp);
        return 0;
    }
    fprintf(stderr, _("error: %s\n"), resp[0] ? resp : _("no running timer"));
    return 1;
}

int main(int argc, char **argv) {
    i18n_init();

    config_t base;
    config_set_defaults(&base);

    char *cfg_path = default_config_path();
    config_ensure_default(cfg_path);
    config_load_file(&base, cfg_path);

    cli_opts_t opts;
    memset(&opts, 0, sizeof opts);
    opts.cfg = base;

    if (!cli_parse(argc, argv, &opts)) {
        free(cfg_path);
        cli_free(&opts);
        return 2;
    }

    const char *save_path = opts.config_path ? opts.config_path : cfg_path;
    int ret = 0;

    switch (opts.action) {
    case ACTION_HELP:
        cli_print_help(argv[0]);
        break;

    case ACTION_VERSION:
        cli_print_version();
        break;

    case ACTION_NONE:
        fprintf(stderr, _("No command given. Try '%s --help'.\n"), argv[0]);
        ret = 1;
        break;

    case ACTION_PAUSE:
        ret = send_control("PAUSE");
        break;
    case ACTION_RESUME:
        ret = send_control("RESUME");
        break;
    case ACTION_STOP:
        ret = send_control("STOP");
        break;

    case ACTION_RECONFIGURE: {
        if (!ipc_is_running()) {
            fprintf(stderr, _("error: no running timer to reconfigure.\n"));
            ret = 1;
            break;
        }
        char msg[1024];
        int nset = cli_build_set_message(&opts, msg, sizeof msg);
        if (nset == 0) {
            fprintf(stderr, _("error: nothing to reconfigure.\n"));
            ret = 1;
            break;
        }
        ret = send_control(msg);
        if (opts.save) {
            if (config_save_file(&opts.cfg, save_path))
                printf(_("Configuration saved to %s\n"), save_path);
        }
        break;
    }

    case ACTION_START_TIMER:
    case ACTION_START_POMODORO: {
        if (ipc_is_running()) {
            fprintf(stderr,
                    _("error: a timer is already running. Stop it first with "
                      "'%s --stop'.\n"),
                    argv[0]);
            ret = 1;
            break;
        }

        if (opts.save) {
            if (config_save_file(&opts.cfg, save_path))
                printf(_("Configuration saved to %s\n"), save_path);
            else
                fprintf(stderr, _("warning: could not save config to %s\n"),
                        save_path);
        }

        printf(_("overlay-timer started.\n"));
        fflush(stdout);

        char *logp = setup_log_path();
        if (!daemonize()) {
            fprintf(stderr, _("error: failed to daemonize.\n"));
            free(logp);
            ret = 1;
            break;
        }
        log_set_file(logp);
        free(logp);

        ret = daemon_run(&opts);
        break;
    }
    }

    free(cfg_path);
    cli_free(&opts);
    return ret;
}
