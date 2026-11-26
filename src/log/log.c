#include "log.h"
#include "ftpsrv_vfs.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
// The log is opened, written (flushed) and closed again for every line, never held open. While a process holds
// a file open for writing, the FS refuses to open it for anyone else (2002-0007, target locked), so holding it
// would stop another process (e.g. an overlay showing the log live) from reading it at all.
// That other process holds it open briefly while reading, so an open (or the delete at start) that's refused is
// retried a few times; lines that still can't be written wait in g_pending and go out with the next line.
#define LOG_OPEN_TRIES 5
#define LOG_RETRY_NS 1000000ULL /* 1 ms */
static FsFileSystem g_fs = {0};
static char g_log_path[FS_MAX_PATH] = {0};
static int g_has_log_file = 0;
static s64 g_file_off = 0;
static char g_pending[2048];
static size_t g_pending_len = 0;

static int log_append(const char* data, size_t len) {
    for (int i = 0; i < LOG_OPEN_TRIES; i++) {
        FsFile file;
        if (R_SUCCEEDED(fsFsOpenFile(&g_fs, g_log_path, FsOpenMode_Write | FsOpenMode_Append, &file))) {
            const Result rc = fsFileWrite(&file, g_file_off, data, len, FsWriteOption_Flush);
            fsFileClose(&file);
            if (R_SUCCEEDED(rc)) {
                g_file_off += len;
                return 1;
            }
            return 0;
        }
        svcSleepThread(LOG_RETRY_NS);
    }
    return 0;
}

void log_file_write(const char* msg) {
    if (g_has_log_file) {
        size_t len = strlen(msg);

        if (len) {
            char buf[256];
            if (msg[len - 1] != '\n') {
                snprintf(buf, sizeof(buf), "%s\n", msg);
                msg = buf;
                len = strlen(msg);
            }

            // A backlog too big to hold is dropped rather than letting the log block the server.
            if (g_pending_len + len > sizeof(g_pending)) {
                g_pending_len = 0;
            }
            if (len > sizeof(g_pending)) {
                return;
            }
            memcpy(g_pending + g_pending_len, msg, len);
            g_pending_len += len;
            if (log_append(g_pending, g_pending_len)) {
                g_pending_len = 0;
            }
        }
    }
}

void log_file_fwrite(const char* fmt, ...) {
    if (g_has_log_file) {
        char buf[256];
        va_list va;
        va_start(va, fmt);
        vsnprintf(buf, sizeof(buf), fmt, va);
        va_end(va);
        log_file_write(buf);
    }
}

void log_file_init(const char* path, const char* init_msg) {
    char safe_buf[FS_MAX_PATH];
    sniprintf(safe_buf, sizeof(safe_buf), "%s", path);

    if (!g_has_log_file) {
        if (R_SUCCEEDED(fsOpenSdCardFileSystem(&g_fs))) {
            // The delete is refused while a reader has the old log open: retry briefly.
            for (int i = 0; i < LOG_OPEN_TRIES; i++) {
                const Result rc = fsFsDeleteFile(&g_fs, safe_buf);
                if (R_SUCCEEDED(rc) || R_DESCRIPTION(rc) == 1 /* path not found */) {
                    break;
                }
                svcSleepThread(LOG_RETRY_NS);
            }
            if (R_SUCCEEDED(fsFsCreateFile(&g_fs, safe_buf, 0, 0))) {
                memcpy(g_log_path, safe_buf, sizeof(g_log_path));
                g_file_off = 0;
                g_pending_len = 0;
                g_has_log_file = 1;
                log_file_write(init_msg);
                return;
            }
            fsFsClose(&g_fs);
        }
    }
}

void log_file_exit(void) {
    if (g_has_log_file) {
        log_file_write("goodbye :)");
        fsFsCommit(&g_fs);
        fsFsClose(&g_fs);
        g_has_log_file = 0;
    }
}
#else
static struct FtpVfsFile g_log_file = {0};
static int g_has_log_file = 0;

void log_file_write(const char* msg) {
    if (g_has_log_file) {
        const size_t len = strlen(msg);
        if (len) {
            ftp_vfs_write(&g_log_file, msg, len);
            if (msg[len - 1] != '\n') {
                ftp_vfs_write(&g_log_file, "\n", 1);
            }
        }
    }
}

void log_file_fwrite(const char* fmt, ...) {
    if (g_has_log_file) {
        char buf[256];
        va_list va;
        va_start(va, fmt);
        vsnprintf(buf, sizeof(buf), fmt, va);
        va_end(va);
        log_file_write(buf);
    }
}

void log_file_init(const char* path, const char* init_msg) {
    if (!g_has_log_file) {
        ftp_vfs_unlink(path);
        g_has_log_file = !ftp_vfs_open(&g_log_file, path, FtpVfsOpenMode_APPEND);
        log_file_write(init_msg);
    }
}

void log_file_exit(void) {
    if (g_has_log_file) {
        log_file_write("goodbye :)");
        ftp_vfs_close(&g_log_file);
        g_has_log_file = 0;
    }
}
#endif
