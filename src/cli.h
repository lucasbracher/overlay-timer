#ifndef OVERLAY_TIMER_CLI_H
#define OVERLAY_TIMER_CLI_H

#include <stdbool.h>
#include "config.h"

/* Main action requested on the command line. */
typedef enum {
    ACTION_NONE = 0,    /* no command given (shows short help) */
    ACTION_START_TIMER, /* -t/--time */
    ACTION_START_POMODORO, /* --pomodoro */
    ACTION_PAUSE,
    ACTION_RESUME,
    ACTION_STOP,
    ACTION_RECONFIGURE, /* only changes options (color/corner/screen...) */
    ACTION_HELP,
    ACTION_VERSION
} cli_action_t;

/* Result of parsing the command line. */
typedef struct {
    cli_action_t action;

    /* Time/pomodoro values. */
    long   time_seconds;   /* ACTION_START_TIMER */
    int    pomodoro_count; /* ACTION_START_POMODORO */

    /* Alternate config path (or NULL). */
    char  *config_path;

    /* true if --save was given. */
    bool   save;

    /* Config with CLI overrides already applied (on top of the base config). */
    config_t cfg;

    /* Bitset: which fields were explicitly set by a flag.
     * Used to send only what changed to the daemon and for --save. */
    struct {
        bool color, alpha, size, font, corner, screen, format;
        bool pomodoro_minutes, break_minutes, long_break_minutes,
             long_break_every, pomodoro_color, break_color;
        bool sounds_dir, enable_sound, backend;
    } set;
} cli_opts_t;

/* Parse argv onto `opts` (whose cfg must already be initialized with the
 * base/default values). Returns true on success; on error prints a message
 * and returns false. */
bool cli_parse(int argc, char **argv, cli_opts_t *opts);

/* Free resources held by opts. */
void cli_free(cli_opts_t *opts);

/* Build an IPC message "SET k=v ..." with the fields marked in opts->set,
 * into `buf`. Returns the number of fields written (0 = nothing to send). */
int cli_build_set_message(const cli_opts_t *opts, char *buf, size_t bufsz);

/* Print full help / version. */
void cli_print_help(const char *prog);
void cli_print_version(void);

#endif /* OVERLAY_TIMER_CLI_H */
