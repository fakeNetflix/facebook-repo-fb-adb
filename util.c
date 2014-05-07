#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/file.h>
#include "util.h"
#include "queue.h"

struct resource {
    enum { RES_RESLIST, RES_CLEANUP } type;
    LIST_ENTRY(resource) link;
};

struct reslist {
    struct resource r;
    struct reslist* parent;
    LIST_HEAD(,resource) contents;
};

struct cleanup {
    struct resource r;
    cleanupfn fn;
    void* fndata;
};

struct errhandler {
    sigjmp_buf where;
    struct reslist* rl;
    struct errinfo* ei;

};

static struct reslist* current_reslist;
static struct errhandler* current_errh;
__attribute__((noreturn)) static void die_oom(void);
const char* prgname;
const char* orig_argv0;

struct reslist*
reslist_push_new(void)
{
    struct reslist* rl = malloc(sizeof (*rl));
    if (rl == NULL)
        die_oom();

    memset(rl, 0, sizeof (*rl));
    rl->r.type = RES_RESLIST;
    rl->parent = current_reslist;
    LIST_INSERT_HEAD(&current_reslist->contents, &rl->r, link);
    current_reslist = rl;
    return rl;
}

void
reslist_pop_nodestroy(void)
{
    current_reslist = current_reslist->parent;
}

void
reslist_cleanup_local(struct reslist** rl_local)
{
    current_reslist = (*rl_local)->parent;
    reslist_destroy(*rl_local);
}

void
reslist_destroy(struct reslist* rl)
{
    if (rl->parent)
        LIST_REMOVE(&rl->r, link);

    while (!LIST_EMPTY(&rl->contents)) {
        struct resource* r = LIST_FIRST(&rl->contents);
        LIST_REMOVE(r, link);
        if (r->type == RES_RESLIST) {
            struct reslist* sub_rl = (struct reslist*) r;
            sub_rl->parent = NULL;
            reslist_destroy(sub_rl);
        } else {
            struct cleanup* cl = (struct cleanup*) r;
            if (cl->fn)
                cl->fn(cl->fndata);
            free(cl);
        }
    }

    free(rl);
}

struct cleanup*
cleanup_allocate(void)
{
    struct cleanup* cl = malloc(sizeof (*cl));
    if (cl == NULL)
        die_oom();

    memset(cl, 0, sizeof (*cl));
    cl->r.type = RES_CLEANUP;
    LIST_INSERT_HEAD(&current_reslist->contents, &cl->r, link);
    return cl;
}

void
cleanup_commit(struct cleanup* cl,
               cleanupfn fn,
               void* fndata)
{
    cl->fn = fn;
    cl->fndata = fndata;
}

static void
fd_cleanup(void* arg)
{
    int fd = (intptr_t) arg;
    if (close(fd) == -1 && errno == EBADF)
        abort();
}

void
cleanup_commit_close_fd(struct cleanup* cl, int fd)
{
    cleanup_commit(cl, fd_cleanup, (void*) (intptr_t) (fd));
}

bool
catch_error(void (*fn)(void* fndata),
            void* fndata,
            struct errinfo* ei)
{
    bool error = true;
    struct errhandler* old_errh = current_errh;
    struct errhandler errh;
    errh.rl = reslist_push_new();
    errh.ei = ei;
    current_errh = &errh;
    if (sigsetjmp(errh.where, 1) == 0) {
        fn(fndata);
        error = false;
    }

    current_errh = old_errh;
    return error;
}

char*
xavprintf(const char* fmt, va_list args)
{
    va_list args2;
    va_copy(args2, args);
    int n = vsnprintf(NULL, 0, fmt, args2);
    va_end(args2);
    if (n < 0)
        die(EINVAL, "invalid format string %.80s", fmt);

    size_t buflen = (size_t) n + 1;
    char* buf = xalloc(buflen);
    vsnprintf(buf, buflen, fmt, args);
    return buf;
}

char*
xaprintf(const char* fmt, ...)
{
    va_list args;
    char* result;
    va_start(args, fmt);
    result = xavprintf(fmt, args);
    va_end(args);
    return result;
}

void*
xalloc(size_t sz)
{
    struct cleanup* cl = cleanup_allocate();
    void* mem = malloc(sz);
    if (mem == NULL)
        die_oom();

    cleanup_commit(cl, free, mem);
    return mem;
}

void*
xcalloc(size_t sz)
{
    void* mem = xalloc(sz);
    memset(mem, 0, sz);
    return mem;
}

void die_oom(void)
{
    if (!current_errh)
        abort();

    current_reslist = current_errh->rl->parent;
    if (current_errh->ei) {
        current_errh->ei->err = ENOMEM;
        current_errh->ei->msg = "no memory";
    }

    reslist_destroy(current_errh->rl);
    siglongjmp(current_errh->where, 1);
}

void
diev(int err, const char* fmt, va_list args)
{
    if (!current_errh)
        abort();

    current_reslist = current_errh->rl->parent;
    /* die_oom will DTRT on alloc failure.  */
    struct errinfo* ei = current_errh->ei;
    if (ei) {
        ei->err = err;
        if (ei->want_msg) {
            ei->msg = xavprintf(fmt, args);
            ei->prgname = xstrdup(prgname);
        }
    }

    reslist_destroy(current_errh->rl);
    siglongjmp(current_errh->where, 1);
}

void
die(int err, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    diev(err, fmt, args);
}

void
die_errno(const char* fmt, ...)
{
    int e = errno;
    va_list args;
    va_start(args, fmt);
    die(e, "%s: %s", xavprintf(fmt, args), strerror(e));
}

int
xopen(const char* pathname, int flags, mode_t mode)
{
    struct cleanup* cl = cleanup_allocate();
    int fd = open(pathname, flags | O_CLOEXEC, mode);
    if (fd == -1)
        die_errno("open(\"%s\")", pathname);

    cleanup_commit_close_fd(cl, fd);
    return fd;
}

void
xpipe(int* read_end, int* write_end)
{
    struct cleanup* cl[2];
    cl[0] = cleanup_allocate();
    cl[1] = cleanup_allocate();

    int fd[2];
    if (pipe2(fd, O_CLOEXEC) < 0)
        die_errno("pipe2");

    cleanup_commit_close_fd(cl[0], fd[0]);
    cleanup_commit_close_fd(cl[1], fd[1]);
    *read_end = fd[0];
    *write_end = fd[1];
}

#define XF_DUPFD_CLOEXEC 1030

int
xdup(int fd)
{
    struct cleanup* cl = cleanup_allocate();
    int newfd = fcntl(fd, XF_DUPFD_CLOEXEC, fd);
    if (newfd == -1)
        die_errno("F_DUPFD_CLOEXEC");

    cleanup_commit_close_fd(cl, newfd);
    return newfd;
}

// Like xdup, but make return a structure that allows the fd to be
// individually closed.
struct fdh*
fdh_dup(int fd)
{
    struct reslist* rl = reslist_push_new();
    struct fdh* fdh = xalloc(sizeof (*fdh));
    fdh->rl = rl;
    fdh->fd = xdup(fd);
    reslist_pop_nodestroy();
    return fdh;
}

void
fdh_destroy(struct fdh* fdh)
{
    reslist_destroy(fdh->rl);
}

struct main_info {
    int argc;
    char** argv;
    int ret;
};

void
main1(void* arg)
{
    struct main_info* mi = arg;
    mi->ret = real_main(mi->argc, mi->argv);
}

int
main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    struct main_info mi;
    mi.argc = argc;
    mi.argv = argv;
    struct reslist dummy_top;
    memset(&dummy_top, 0, sizeof (dummy_top));
    current_reslist = &dummy_top;
    prgname = argv[0];
    struct reslist* top_rl = reslist_push_new();
    orig_argv0 = argv[0];
    prgname = strdup(basename(xstrdup(argv[0])));
    struct errinfo ei = { .want_msg = true };
    if (catch_error(main1, &mi, &ei)) {
        mi.ret = 1;
        fprintf(stderr, "%s: %s\n", ei.prgname, ei.msg);
    }

    reslist_destroy(top_rl);
    return mi.ret;
}

// Round up to next power of two.  If zero given as input, return 0.
// If number too large to fit, return 0.
size_t
nextpow2sz(size_t sz)
{
    sz -= 1;
    sz |= sz >> 1;
    sz |= sz >> 2;
    sz |= sz >> 4;
    sz |= sz >> 8;
    sz |= sz >> 16;
    if (sizeof (sz) == 8)
        sz |= sz >> 32;

    return 1 + sz;
}

char*
xstrdup(const char* s)
{
    return xaprintf("%s", s);
}

static void
cleanup_prgname(void* arg)
{
    prgname = arg;
}

void
set_prgname(const char* s)
{
    struct cleanup* c = cleanup_allocate();
    cleanup_commit(c, cleanup_prgname, (void*) prgname);
    prgname = s;
}

size_t
iovec_sum(const struct iovec* iov, unsigned niovec)
{
    size_t total = 0;
    for (unsigned i = 0; i < niovec; ++i)
        total += iov[i].iov_len;

    return total;
}

enum blocking_mode
fd_get_blocking_mode(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        die_errno("fcntl(%d, F_GETFL)", fd);

    return (flags & O_NONBLOCK) ? non_blocking : blocking;
}

enum blocking_mode
fd_set_blocing_mode(int fd, enum blocking_mode mode)
{
    int flags = fcntl(fd, F_GETFL);
    enum blocking_mode old_mode;
    if (flags < 0)
        die_errno("fcntl(%d, F_GETFL)", fd);

    old_mode = (flags & O_NONBLOCK) ? non_blocking : blocking;
    if (mode == non_blocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    if (fcntl(fd, F_SETFL, flags) < 0)
        die_errno("fcntl(%d, F_SETFL, %x)", fd, flags);

    return old_mode;
}

void
dbg(const char* fmt, ...)
{
    SCOPED_RESLIST(rl_dbg);
    dbglock();
    va_list args;
    fprintf(stderr, "%s(%04d): ", prgname, getpid());
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static int dbglock_fd = -1;
static int dbglock_level = 0;

static void
cleanup_dbglock(void* ignored)
{
    if (--dbglock_level == 0)
        flock(dbglock_fd, LOCK_UN);
}

void
dbglock(void)
{
    if (dbglock_fd == -1)
        dbglock_fd = open("/tmp/adbx.lock",
                          O_RDWR | O_CREAT | O_CLOEXEC,
                          0644);

    struct cleanup* cl = cleanup_allocate();
    if (dbglock_level++ == 0)
        flock(dbglock_fd, LOCK_EX);
    cleanup_commit(cl, cleanup_dbglock, 0);
}

void
hack_reopen_tty(int fd)
{
    // We sometimes need O_NONBLOCK on our input and output streams,
    // but O_NONBLOCK applies to the entire file object.  If the file
    // object happens to be a tty we've inherited, everything that
    // uses that tty will start getting EAGAIN and all hell will break
    // loose.  Here, we reopen the tty so we can get a fresh file
    // object and control the blocking mode separately.
    SCOPED_RESLIST(rl_hack);
    int nfd = xopen(ttyname(fd), O_RDWR, 0);
    if (dup3(nfd, fd, O_CLOEXEC) < 0)
        die_errno("dup3");
}
