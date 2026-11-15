#ifndef OVERLAY_TIMER_DAEMON_H
#define OVERLAY_TIMER_DAEMON_H

#include "cli.h"

/*
 * Background process (daemon) that keeps the timer and serves commands via IPC.
 *
 * Must be called AFTER daemonize(). Assumes that the single instance has already
 * been guaranteed (ipc_server_start returned 0). `opts` contains the initial
 * action (START_TIMER or START_POMODORO) and the effective configuration.
 *
 * Returns the process exit code.
 */
int daemon_run(const cli_opts_t *opts);

#endif /* OVERLAY_TIMER_DAEMON_H */
