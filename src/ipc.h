#ifndef OVERLAY_TIMER_IPC_H
#define OVERLAY_TIMER_IPC_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Single-instance IPC via Unix socket + lockfile.
 *
 * - The lockfile lives at $XDG_RUNTIME_DIR/overlay-timer.lock (exclusive flock).
 * - The socket lives at $XDG_RUNTIME_DIR/overlay-timer.sock.
 *
 * Protocol (text, one line per message, terminated by '\n'):
 *   Client -> server:  command [args]
 *   Server -> client:  "OK[ text]"  or  "ERR message"
 */

/* Paths (allocated string; caller frees). */
char *ipc_socket_path(void);
char *ipc_lock_path(void);

/* --------------------------- Server side --------------------------- */

typedef struct {
    int listen_fd; /* listening socket */
    int lock_fd;   /* lockfile fd (holds the flock) */
    char *sock_path;
    char *lock_path;
} ipc_server_t;

/* Try to become the single instance:
 *   - acquire the exclusive lock (non-blocking);
 *   - create and listen on the socket.
 * Returns:
 *    0  success (srv filled in);
 *    1  an instance is already running;
 *   -1  system error (see log).
 */
int ipc_server_start(ipc_server_t *srv);

/* Close the socket, remove files and release the lock. */
void ipc_server_stop(ipc_server_t *srv);

/* Accept a connection and read a command line.
 * Returns a connected client fd (>=0) with *line filled in (without '\n'),
 * or -1 if there is no connection ready / error. The caller must reply with
 * ipc_server_reply() and then close the fd. */
int ipc_server_accept(ipc_server_t *srv, char *line, size_t linesz);

/* Send a response to the client and close the fd. */
void ipc_server_reply(int client_fd, const char *response);

/* --------------------------- Client side --------------------------- */

/* Check whether an instance is running (tries to connect to the socket). */
bool ipc_is_running(void);

/* Connect, send `msg` (appends '\n'), read the response into `resp`.
 * Returns true if the response starts with "OK"; false otherwise
 * (including connection failure). In any case, if resp!=NULL it receives the
 * textual response (or error message). */
bool ipc_client_request(const char *msg, char *resp, size_t resp_sz);

/* --------------------------- Daemonization --------------------------- */

/* Does a double fork + setsid, redirects stdio to /dev/null and returns in the
 * child process in the background. Returns true in the child process (daemon);
 * the parent process exits with exit(0) inside this function. Returns false on error. */
bool daemonize(void);

#endif /* OVERLAY_TIMER_IPC_H */
