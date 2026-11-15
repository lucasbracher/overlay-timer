#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "i18n.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* Paths                                                              */
/* ------------------------------------------------------------------ */

/* Creates a close-on-exec Unix stream socket (portable: SOCK_CLOEXEC is
 * Linux-only). */
static int unix_socket(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

static char *runtime_path(const char *name) {
    char *dir = xdg_runtime_dir();
    /* Ensure the directory exists (the /tmp/... fallback may not exist). */
    mkdirs(dir, 0700);
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char *out = xmalloc(n);
    snprintf(out, n, "%s/%s", dir, name);
    free(dir);
    return out;
}

char *ipc_socket_path(void) { return runtime_path("overlay-timer.sock"); }
char *ipc_lock_path(void)   { return runtime_path("overlay-timer.lock"); }

/* ------------------------------------------------------------------ */
/* Socket helpers                                                     */
/* ------------------------------------------------------------------ */

static bool fill_sockaddr(struct sockaddr_un *addr, const char *path) {
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr->sun_path) {
        log_error(_("socket path too long: %s"), path);
        return false;
    }
    strncpy(addr->sun_path, path, sizeof addr->sun_path - 1);
    return true;
}

/* Read a line (up to '\n' or EOF) from the fd. Strips the '\n'. */
static ssize_t read_line(int fd, char *buf, size_t bufsz) {
    size_t i = 0;
    while (i + 1 < bufsz) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r == 0) break;              /* EOF */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ch == '\n') break;
        buf[i++] = ch;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

static bool write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Server                                                             */
/* ------------------------------------------------------------------ */

int ipc_server_start(ipc_server_t *srv) {
    memset(srv, 0, sizeof *srv);
    srv->listen_fd = -1;
    srv->lock_fd = -1;
    srv->lock_path = ipc_lock_path();
    srv->sock_path = ipc_socket_path();

    /* 1. Exclusive non-blocking lockfile. */
    srv->lock_fd = open(srv->lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (srv->lock_fd < 0) {
        log_error(_("cannot open lockfile %s: %s"), srv->lock_path, strerror(errno));
        goto fail;
    }
    if (flock(srv->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            ipc_server_stop(srv);
            return 1; /* an instance already exists */
        }
        log_error(_("flock failed: %s"), strerror(errno));
        goto fail;
    }

    /* Write the PID into the lockfile (informational). */
    if (ftruncate(srv->lock_fd, 0) == 0) {
        char pidbuf[32];
        int n = snprintf(pidbuf, sizeof pidbuf, "%ld\n", (long)getpid());
        if (write_all(srv->lock_fd, pidbuf, (size_t)n)) { /* ok */ }
    }

    /* 2. Remove any orphan socket and create the new one. */
    unlink(srv->sock_path);

    srv->listen_fd = unix_socket();
    if (srv->listen_fd < 0) {
        log_error(_("socket() failed: %s"), strerror(errno));
        goto fail;
    }

    struct sockaddr_un addr;
    if (!fill_sockaddr(&addr, srv->sock_path)) goto fail;

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        log_error(_("bind() failed on %s: %s"), srv->sock_path, strerror(errno));
        goto fail;
    }
    if (listen(srv->listen_fd, 8) != 0) {
        log_error(_("listen() failed: %s"), strerror(errno));
        goto fail;
    }

    return 0;

fail:
    ipc_server_stop(srv);
    return -1;
}

void ipc_server_stop(ipc_server_t *srv) {
    if (!srv) return;
    if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }
    if (srv->sock_path) { unlink(srv->sock_path); }
    if (srv->lock_fd >= 0) {
        flock(srv->lock_fd, LOCK_UN);
        close(srv->lock_fd);
        srv->lock_fd = -1;
    }
    if (srv->lock_path) { unlink(srv->lock_path); }
    free(srv->sock_path); srv->sock_path = NULL;
    free(srv->lock_path); srv->lock_path = NULL;
}

int ipc_server_accept(ipc_server_t *srv, char *line, size_t linesz) {
    int fd = accept(srv->listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            log_warn(_("accept() failed: %s"), strerror(errno));
        return -1;
    }
    if (read_line(fd, line, linesz) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void ipc_server_reply(int client_fd, const char *response) {
    if (client_fd < 0) return;
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%s\n", response ? response : "OK");
    write_all(client_fd, buf, (size_t)n);
    close(client_fd);
}

/* ------------------------------------------------------------------ */
/* Client                                                             */
/* ------------------------------------------------------------------ */

static int client_connect(void) {
    char *path = ipc_socket_path();
    int fd = unix_socket();
    if (fd < 0) { free(path); return -1; }

    struct sockaddr_un addr;
    if (!fill_sockaddr(&addr, path)) { close(fd); free(path); return -1; }

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        free(path);
        return -1;
    }
    free(path);
    return fd;
}

bool ipc_is_running(void) {
    int fd = client_connect();
    if (fd >= 0) { close(fd); return true; }
    return false;
}

bool ipc_client_request(const char *msg, char *resp, size_t resp_sz) {
    if (resp && resp_sz) resp[0] = '\0';

    int fd = client_connect();
    if (fd < 0) {
        if (resp && resp_sz) snprintf(resp, resp_sz, _("no running timer"));
        return false;
    }

    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%s\n", msg);
    if (!write_all(fd, buf, (size_t)n)) {
        close(fd);
        if (resp && resp_sz) snprintf(resp, resp_sz, _("write failed"));
        return false;
    }

    char line[1024];
    ssize_t r = read_line(fd, line, sizeof line);
    close(fd);
    if (r < 0) {
        if (resp && resp_sz) snprintf(resp, resp_sz, _("read failed"));
        return false;
    }

    if (resp && resp_sz) snprintf(resp, resp_sz, "%s", line);
    return strncmp(line, "OK", 2) == 0;
}

/* ------------------------------------------------------------------ */
/* Daemonization                                                      */
/* ------------------------------------------------------------------ */

bool daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { log_error(_("fork failed: %s"), strerror(errno)); return false; }
    if (pid > 0) _exit(0); /* parent exits, returns the terminal */

    if (setsid() < 0) { log_error(_("setsid failed: %s"), strerror(errno)); return false; }

    /* Second fork to ensure we do not reacquire a controlling terminal. */
    pid = fork();
    if (pid < 0) { log_error(_("fork failed: %s"), strerror(errno)); return false; }
    if (pid > 0) _exit(0);

    umask(0077);
    if (chdir("/") != 0) { /* ignore */ }

    /* Redirect stdio to /dev/null. */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }
    return true;
}
