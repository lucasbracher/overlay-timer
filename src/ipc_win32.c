/*
 * Windows implementation of ipc.h.
 *
 * Same protocol and behavior as ipc.c, with these differences:
 * - Sockets: AF_UNIX through Winsock (Windows 10 1803+). Socket handles are
 *   stored in the `int` fields of ipc_server_t (they fit in 32 bits).
 * - Single instance: a named mutex ("Local\overlay-timer") instead of flock
 *   on a lockfile.
 * - daemonize(): there is no fork(), so the program re-launches itself as a
 *   detached process (no console) and the parent exits. The child is marked
 *   with the OVERLAY_TIMER_DAEMON=1 environment variable.
 */
#include "ipc.h"

#include <winsock2.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<afunix.h>)
#include <afunix.h>
#define HAVE_AFUNIX_H 1
#endif
#endif
#ifndef HAVE_AFUNIX_H
#define UNIX_PATH_MAX 108
struct sockaddr_un {
    ADDRESS_FAMILY sun_family;
    char sun_path[UNIX_PATH_MAX];
};
#endif

#include "i18n.h"
#include "util.h"

#define DAEMON_ENV "OVERLAY_TIMER_DAEMON"
#define MUTEX_NAME L"Local\\overlay-timer"

static HANDLE g_mutex = NULL;

/* Formats the last Winsock error as a short string (static buffer). */
static const char *wsa_err(void) {
    static char buf[32];
    snprintf(buf, sizeof buf, "WSA error %d", WSAGetLastError());
    return buf;
}

static const char *win_err(void) {
    static char buf[32];
    snprintf(buf, sizeof buf, "error %lu", (unsigned long)GetLastError());
    return buf;
}

static bool ensure_wsa(void) {
    static bool done = false;
    if (done) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    done = true;
    return true;
}

/* Creates a non-inheritable AF_UNIX stream socket, or -1. */
static int unix_socket(void) {
    if (!ensure_wsa()) return -1;
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    return (int)s;
}

static void close_sock(int fd) {
    if (fd >= 0) closesocket((SOCKET)fd);
}

/* ------------------------------------------------------------------ */
/* Paths                                                              */
/* ------------------------------------------------------------------ */

static char *runtime_path(const char *name) {
    char *dir = xdg_runtime_dir();
    mkdirs(dir, 0700);
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char *out = xmalloc(n);
    snprintf(out, n, "%s/%s", dir, name);
    free(dir);
    return out;
}

char *ipc_socket_path(void) { return runtime_path("overlay-timer.sock"); }
/* Kept for API compatibility; Windows uses a named mutex, not a lockfile. */
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

/* Read a line (up to '\n' or EOF) from the socket. Strips the '\n'. */
static int read_line(int fd, char *buf, size_t bufsz) {
    size_t i = 0;
    while (i + 1 < bufsz) {
        char ch;
        int r = recv((SOCKET)fd, &ch, 1, 0);
        if (r == 0) break;      /* EOF */
        if (r < 0) return -1;
        if (ch == '\n') break;
        buf[i++] = ch;
    }
    buf[i] = '\0';
    return (int)i;
}

static bool write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int w = send((SOCKET)fd, data + off, (int)(len - off), 0);
        if (w < 0) return false;
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
    srv->lock_path = NULL;
    srv->sock_path = ipc_socket_path();

    /* 1. Single instance: named mutex, released when the process exits. */
    g_mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (!g_mutex) {
        log_error(_("cannot create mutex: %s"), win_err());
        goto fail;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ipc_server_stop(srv);
        return 1; /* an instance already exists */
    }

    /* 2. Remove any orphan socket and create the new one. */
    DeleteFileA(srv->sock_path);

    srv->listen_fd = unix_socket();
    if (srv->listen_fd < 0) {
        log_error(_("socket() failed: %s"), wsa_err());
        goto fail;
    }

    struct sockaddr_un addr;
    if (!fill_sockaddr(&addr, srv->sock_path)) goto fail;

    if (bind((SOCKET)srv->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        log_error(_("bind() failed on %s: %s"), srv->sock_path, wsa_err());
        goto fail;
    }
    if (listen((SOCKET)srv->listen_fd, 8) != 0) {
        log_error(_("listen() failed: %s"), wsa_err());
        goto fail;
    }

    return 0;

fail:
    ipc_server_stop(srv);
    return -1;
}

void ipc_server_stop(ipc_server_t *srv) {
    if (!srv) return;
    if (srv->listen_fd >= 0) { close_sock(srv->listen_fd); srv->listen_fd = -1; }
    if (srv->sock_path) DeleteFileA(srv->sock_path);
    if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
    free(srv->sock_path); srv->sock_path = NULL;
    free(srv->lock_path); srv->lock_path = NULL;
}

int ipc_server_accept(ipc_server_t *srv, char *line, size_t linesz) {
    SOCKET c = accept((SOCKET)srv->listen_fd, NULL, NULL);
    if (c == INVALID_SOCKET) {
        if (WSAGetLastError() != WSAEWOULDBLOCK)
            log_warn(_("accept() failed: %s"), wsa_err());
        return -1;
    }
    int fd = (int)c;
    if (read_line(fd, line, linesz) < 0) {
        close_sock(fd);
        return -1;
    }
    return fd;
}

void ipc_server_reply(int client_fd, const char *response) {
    if (client_fd < 0) return;
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%s\n", response ? response : "OK");
    write_all(client_fd, buf, (size_t)n);
    close_sock(client_fd);
}

/* ------------------------------------------------------------------ */
/* Client                                                             */
/* ------------------------------------------------------------------ */

static int client_connect(void) {
    char *path = ipc_socket_path();
    int fd = unix_socket();
    if (fd < 0) { free(path); return -1; }

    struct sockaddr_un addr;
    if (!fill_sockaddr(&addr, path)) { close_sock(fd); free(path); return -1; }

    if (connect((SOCKET)fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close_sock(fd);
        free(path);
        return -1;
    }
    free(path);
    return fd;
}

bool ipc_is_running(void) {
    int fd = client_connect();
    if (fd >= 0) { close_sock(fd); return true; }
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
        close_sock(fd);
        if (resp && resp_sz) snprintf(resp, resp_sz, _("write failed"));
        return false;
    }

    char line[1024];
    int r = read_line(fd, line, sizeof line);
    close_sock(fd);
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
    /* Child: already detached by the parent below. */
    const char *flag = getenv(DAEMON_ENV);
    if (flag && strcmp(flag, "1") == 0) {
        SetEnvironmentVariableW(L"" DAEMON_ENV, NULL); /* do not leak to children */
        return true;
    }

    /* Parent: re-launch ourselves with the same arguments, without a console. */
    wchar_t exe[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        log_error(_("CreateProcess failed: %s"), win_err());
        return false;
    }

    wchar_t *cmdline = _wcsdup(GetCommandLineW()); /* must be writable */
    if (!cmdline) return false;

    SetEnvironmentVariableW(L"" DAEMON_ENV, L"1");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    BOOL ok = CreateProcessW(exe, cmdline, NULL, NULL, FALSE,
                             DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                             NULL, NULL, &si, &pi);
    free(cmdline);
    if (!ok) {
        log_error(_("CreateProcess failed: %s"), win_err());
        SetEnvironmentVariableW(L"" DAEMON_ENV, NULL);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    exit(0); /* parent exits, returns the terminal */
}
