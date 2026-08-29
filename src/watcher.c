#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define WATCH_DIR "./example-dir"

typedef struct {
    int32_t fan_fd;
    int32_t mnt_fd;
} watcher;

/// Stub for interfacing with a work queue.
///
/// In practice, this would do event handoff to the indexer, which makes `statx`
/// calls to diff against existing state.
typedef enum {
    SYNC_CONTENT_CHANGED, // emit hint to indexer
    SYNC_ENTRY_REMOVED, // manifest deletion
    SYNC_ENTRY_MOVED_IN, // create/modify at new path
    SYNC_FULL_RESCAN // kernel queue overflow and should reconcile actual state
} sync_action;

/// Mocks event enqueuer functionality: as per above, we might do  handoff to
/// the indexer via an MPSC message or something like this.
static void enqueue(sync_action action, const char *path) {
    static const char *names[] =
        {"CONTENT_CHANGED", "ENTRY_REMOVED", "MOVED_IN", "FULL_RESCAN"};

    printf("[queue] %-15s %s\n", names[action], path ? path : "(all)");
}

static void watcher_clean(watcher *w) {
    printf("running cleanup\n");
    if (w->mnt_fd) {
        close(w->mnt_fd);
    }

    if (w->fan_fd) {
        close(w->fan_fd);
    }
}

static int watcher_init(watcher *w, const char *mnt_path) {
    int result;

    // `FAN_REPORT_DFID_NAME` is "shorthand" for
    // `FAN_REPORT_DIR_FID | FAN_REPORT_NAME`
    w->fan_fd = fanotify_init(
        FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME | FAN_NONBLOCK,
        O_CLOEXEC | O_RDONLY
    );

    if (w->fan_fd == -1) {
        perror("fanotify_init");
        result = -1;
        goto _ep;
    }

    w->mnt_fd = open(mnt_path, O_DIRECTORY | O_CLOEXEC);
    if (w->mnt_fd == -1) {
        perror("open");

        result = -1;
        goto _ep;
    }

    if (fanotify_mark(
            w->fan_fd,
            FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
            FAN_CLOSE_WRITE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO
                | FAN_ONDIR,
            AT_FDCWD,
            WATCH_DIR
        )
        < 0) {
        perror("fanotify_mark");

        result = -1;
        goto _ep;
    }

    result = 0;
_ep:
    // handle any cleanup on failure
    if (result == -1) {
        watcher_clean(w);
    } else {
        printf("opened: fan_fd=%d, mnt_fd=%d\n", w->fan_fd, w->mnt_fd);
        fprintf(
            stderr,
            "mount_fd=%d valid=%d\n",
            w->mnt_fd,
            fcntl(w->mnt_fd, F_GETFD) >= 0
        );
    }

    return result;
}

static int watcher_resolve_path(
    const watcher *w,
    const struct file_handle *fh,
    const char *name,
    char *out,
    size_t out_len
) {
    char proc[64];

    int fd = open_by_handle_at(
        w->mnt_fd,
        (struct file_handle *)fh,
        O_PATH | O_CLOEXEC
    );

    printf("fd -> %d\n", fd);

    if (fd < 0) {
        return -1;
    }

    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, out, out_len - 1);

    close(fd);
    if (n < 0) {
        return -1;
    }

    out[n] = '\0';

    if (name && name[0] && strcmp(name, ".") != 0) {
        size_t used = (size_t)n;
        if (used + 1 + strlen(name) + 1 > out_len) {
            errno = ENAMETOOLONG;
            return -1;
        }

        out[used] = '/';
        strcpy(out + used + 1, name);
    }

    return 0;
}

static void
handle_event(const watcher *w, const struct fanotify_event_metadata *md) {
    const struct fanotify_event_info_fid *fid = NULL;
    const struct fanotify_event_info_header *ih;
    const struct file_handle *fh;
    char *name;
    char path[PATH_MAX + 1];

    // kernel queue overflow, which is generally indicative of having lost
    // events; we would want to do a full rescan to reconcile lost dir state
    if (md->mask & FAN_Q_OVERFLOW) {
        enqueue(SYNC_FULL_RESCAN, NULL);
        return;
    }

    for (ih = (const void *)(md + 1);
         (const char *)ih < (const char *)md + md->event_len;
         ih = (const void *)((const char *)ih + ih->len)) {
        if (ih->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
            fid = (struct fanotify_event_info_fid *)ih;
            break;
        }
    }

    if (!fid) {
        return;
    }

    fh = (const struct file_handle *)fid->handle;
    name = (char *)fh->f_handle + fh->handle_bytes;

    printf("fh=%p,name=%s\n", fh, name);

    if (watcher_resolve_path(w, fh, name, path, sizeof(path)) < 0) {
        if (errno != ESTALE) {
            perror("watcher_resolve_path");
        }
        return;
    }

    if (md->mask & FAN_CLOSE_WRITE) {
        enqueue(SYNC_CONTENT_CHANGED, path);
    }
    if (md->mask & (FAN_DELETE | FAN_MOVED_FROM)) {
        enqueue(SYNC_ENTRY_REMOVED, path);
    }
    if (md->mask & FAN_MOVED_TO) {
        enqueue(SYNC_ENTRY_MOVED_IN, path);
    }
}

static void watcher_drain(const watcher *w) {
    ssize_t len;
    char buf[8192] __attribute__((aligned(8)));
    const struct fanotify_event_metadata *md;

    for (;;) {
        len = read(w->fan_fd, buf, sizeof buf);
        if (len < 0) {
            if (errno == EAGAIN) {
                // fully drained
                return;
            }

            if (errno == EINTR) {
                continue;
            }

            perror("read fanotify");
            return;
        }

        // --------------------------------------------------------------------
        // TODO not a huge fan of having the side effect below in this function
        // and i would prefer to return the buf (or metadata or whatever) and
        // read through events in the caller rather than here.
        // --------------------------------------------------------------------

        md = (const void *)buf;
        printf("got event: len=%lu\n", len);

        while (FAN_EVENT_OK(md, len)) {
            if (md->vers != FANOTIFY_METADATA_VERSION) {
                fprintf(stderr, "fanotify ABI mismatch\n");
                exit(EXIT_FAILURE);
            }

            handle_event(w, md);
            md = FAN_EVENT_NEXT(md, len);
        }
    }
}

int main(void) {
    struct epoll_event ev;
    int ep;

    watcher w = {
        .fan_fd = -1,
        .mnt_fd = -1,
    };

    if ((watcher_init(&w, WATCH_DIR)) != 0) {
        printf("`fanotify` instance init failure\n");
        return EXIT_FAILURE;
    }

    ep = epoll_create1(EPOLL_CLOEXEC);

    ev.events = EPOLLIN;
    ev.data.fd = w.fan_fd;

    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, w.fan_fd, &ev) < 0) {
        perror("epoll");
        return EXIT_FAILURE;
    }

    printf("watching: %s\n", WATCH_DIR);
    for (;;) {
        struct epoll_event out[4];
        int n = epoll_wait(ep, out, 4, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (out[i].data.fd == w.fan_fd) {
                watcher_drain(&w);
            }
        }
    }

    close(ep);
    watcher_clean(&w);

    return EXIT_SUCCESS;
}
