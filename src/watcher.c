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

/// Stub for interfacing with a work queue. In practice, this would do event
/// handoff to the indexer, which makes `statx` calls to diff against existing
/// state
typedef enum {
    /// File content changed hint
    SYNC_CONTENT_CHANGED,

    // Manifest deletion hint
    SYNC_ENTRY_REMOVED,

    // Recreate at new path hint
    SYNC_ENTRY_MOVED_IN,

    // Reconciliation scan hint
    SYNC_FULL_RESCAN
} sync_action;

typedef struct {
    char *buf;
    size_t len;
} watch_dir;

typedef struct {
    int32_t fan_fd;
    int32_t mnt_fd;
    watch_dir dir;
} watcher;

/// Mocks event enqueuer functionality: as per above, we might do  handoff to
/// the indexer via an MPSC message (or something like this)
static void enqueue(sync_action action, const char *path) {
    static const char *names[] =
        {"CONTENT_CHANGED", "ENTRY_REMOVED", "MOVED_IN", "FULL_RESCAN"};

    printf("[queue] %-15s %s\n", names[action], path ? path : "(all)");
}

/// Runs `watcher` file descriptor cleanup
static void watcher_clean(watcher *w) {
    printf("running cleanup\n");
    if (w->mnt_fd) {
        close(w->mnt_fd);
    }

    if (w->fan_fd) {
        close(w->fan_fd);
    }
}

/// Initializes a file descriptors in a `watcher` struct and adds a mark to the
/// fanotify instance. Initialization is performed by populating the relevant
/// file descriptors `fan_fd` & `mnt_fd` via the relevant syscalls.
static int watcher_init(watcher *w, const char *mnt_path) {
    int result;

    // open fanotify instance
    if ((w->fan_fd = fanotify_init(
             FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME | FAN_NONBLOCK,
             O_CLOEXEC | O_RDONLY
         ))
        == -1)
    {
        perror("fanotify_init");

        result = -1;
        goto _ep;
    }

    // open filesystem for reading
    if ((w->mnt_fd = open(mnt_path, O_DIRECTORY | O_CLOEXEC)) == -1) {
        perror("open");

        result = -1;
        goto _ep;
    }

    // add a filesystem-wide mark
    if (fanotify_mark(
            w->fan_fd,
            FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
            FAN_CLOSE_WRITE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO
                | FAN_ONDIR,
            AT_FDCWD,
            mnt_path
        )
        < 0)
    {
        perror("fanotify_mark");

        result = -1;
        goto _ep;
    }

    result = 0;

_ep:
    if (result == -1) {
        watcher_clean(w);
    }

    return result;
}

/// Resolves a file handle to a directory into its path. ESTALE is occasionally
/// expected, and so shouldn't be fatal - the entry was deleted/renamed before
/// we could resolve it, which is intended to be reconciled via periodic rescans
static int watcher_resolve_path(
    const watcher *w,
    const struct file_handle *fh,
    const char *name,
    char *out,
    size_t out_len
) {
    char proc[64];
    int fd;
    ssize_t n;
    size_t used;

    fd = open_by_handle_at(
        w->mnt_fd,
        (struct file_handle *)fh,
        O_PATH | O_CLOEXEC
    );

    if (fd < 0) {
        return -1;
    }

    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    n = readlink(proc, out, out_len - 1);

    close(fd);
    if (n < 0) {
        return -1;
    }

    out[n] = '\0';
    if (name && name[0] && strcmp(name, ".") != 0) {
        used = (size_t)n;
        if (used + 1 + strlen(name) + 1 > out_len) {
            errno = ENAMETOOLONG;
            return -1;
        }

        out[used] = '/';
        strcpy(out + used + 1, name);
    }

    return 0;
}

/// Check whether an `event` filepath starts with the `char *` `target`
static int
filter_by_prefix(const char *target, const char *event, const size_t length) {
    if (strlen(event) < length || (memcmp(target, event, length)) != 0) {
        return 0;
    }

    return 1;
}

/// Handle a `fanotify` event, using the event metadata to retrieve DFID_NAME
/// records for a given event.
static void
handle_event(const watcher *w, const struct fanotify_event_metadata *md) {
    const struct fanotify_event_info_fid *fid = NULL;
    const struct fanotify_event_info_header *ih;
    const struct file_handle *fh;
    char *name;
    char path[PATH_MAX + 1];

    // kernel queue overflow - we have missed events and should reconcile state
    // via a full rescan
    if (md->mask & FAN_Q_OVERFLOW) {
        enqueue(SYNC_FULL_RESCAN, NULL);
        return;
    }

    for (const char *p = (((const char *)md) + md->metadata_len);
         p + sizeof *ih <= (((const char *)md) + md->event_len);
         p += ih->len)
    {
        // update current `ih->len` for next iteration
        ih = (const struct fanotify_event_info_header *)p;
        if (ih->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
            fid = (struct fanotify_event_info_fid *)p;
            break;
        }
    }

    if (!fid) {
        return;
    }

    fh = (const struct file_handle *)fid->handle;
    name = (char *)fh->f_handle + fh->handle_bytes;
    if (watcher_resolve_path(w, fh, name, path, sizeof(path)) < 0) {
        if (errno != ESTALE) {
            perror("watcher_resolve_path");
        }
        return;
    }

    // we might do something like cache this by its file handle so we avoid
    // calling (the comparatively expensive) `watcher_resolve_path` above, but
    // this Also Works
    if (!(filter_by_prefix(w->dir.buf, path, w->dir.len))) {
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

/// Drain everything currently readable on the `fanotify` instance. Called by
/// `epoll` when it reports that the fd is readable
static void watcher_drain(const watcher *w) {
    ssize_t len;
    char buf[8192] __attribute__((aligned(8)));
    const struct fanotify_event_metadata *md;

    while (1) {
        len = read(w->fan_fd, buf, sizeof buf);
        if (len < 0) {
            if (errno == EAGAIN) {
                return; // fully drained
            }
            if (errno == EINTR) {
                continue;
            }

            perror("read fanotify");
            return;
        }

        md = (const void *)buf;

        while (FAN_EVENT_OK(md, len)) {
            if (md->vers != FANOTIFY_METADATA_VERSION) {
                fprintf(stderr, "fanotify ABI mismatch\n");
                exit(EXIT_FAILURE);
            }
            // TODO: not a huge fan of this side effect but it stays for now...
            handle_event(w, md);
            md = FAN_EVENT_NEXT(md, len);
        }
    }
}

int main(void) {
    watch_dir watchdir;
    int ep;
    struct epoll_event ev;

    // we just use the hardcoded directory at the top of the file
    if ((watchdir.buf = realpath(WATCH_DIR, NULL)) == NULL) {
        perror("realpath");
        return EXIT_FAILURE;
    }

    watchdir.len = strlen(watchdir.buf);

    watcher w = {
        .dir = watchdir,
        .fan_fd = -1,
        .mnt_fd = -1,
    };

    if ((watcher_init(&w, watchdir.buf)) != 0) {
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

    printf("watching: %s\n", watchdir.buf);
    while (1) {
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
