/*
 * shell.c - the stable half of a hot-swappable QNX driver.
 *
 * The shell is a QNX resource manager that owns /dev/counter. It holds no
 * driver logic: every read() is forwarded to the current "core" process and
 * the core's answer is passed back to the client.
 *
 * Swap: a new core version announces itself with DCMD_HOTSWAP_REGISTER. The
 * shell takes the state out of the old core (in a format the new core can
 * import), gives it to the new core and switches over. The shell is
 * single-threaded, so while the swap runs, client requests wait in the
 * kernel on the shell's channel (SEND-blocked) and are served by the new
 * core as soon as the devctl handler returns. Clients never see an error and
 * keep their file descriptors.
 *
 * Probation and rollback: the old core is not retired at once. It stays
 * parked (alive, not serving) for a probation period. If the new core fails
 * during that time, the shell gives the old core back its own state snapshot
 * and retries the failed request there, so the client still sees no error.
 * After the probation period the swap is committed and the old core exits.
 * Limitation: anything the new core changed before failing is lost; the
 * old core resumes from the handover snapshot.
 *
 * Usage: shell [-t probation_ms]   (default 2000; 0 = retire immediately)
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "hotswap_proto.h"

/* A connection to one core process. coid == -1 means "none". */
struct core_ref {
    int      coid;
    pid_t    pid;
    uint32_t version;
};

static struct core_ref   cur    = { .coid = -1 };  /* serves reads */
static struct core_ref   parked = { .coid = -1 };  /* predecessor on probation */
static struct core_state parked_snapshot;          /* its own state at handover */
static uint64_t          probation_end_ns;
static uint64_t          probation_ns = 2000ull * 1000000ull;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Send one request to a core and wait for its reply. Returns 0 or -1/errno. */
static int core_call(int coid, enum core_op op, const struct core_state *in,
                     void *reply, size_t reply_len)
{
    struct core_msg m;
    memset(&m, 0, sizeof m);
    m.type = CORE_MSG_TYPE;
    m.op   = (uint16_t)op;
    if (in) {
        m.state = *in;
    }
    return MsgSend(coid, &m, sizeof m, reply, reply_len) == -1 ? -1 : 0;
}

/* Ask a core for its state in the newest format it can write that is not
 * newer than 'wanted'. */
static int core_export(int coid, uint32_t wanted, struct core_state *out)
{
    struct core_state req = { .format = wanted };
    return core_call(coid, CORE_OP_EXPORT, &req, out, sizeof *out);
}

/* Tell a core to exit and drop our connection to it. */
static void retire(struct core_ref *c)
{
    if (c->coid != -1) {
        core_call(c->coid, CORE_OP_QUIT, NULL, NULL, 0);
        ConnectDetach(c->coid);
        c->coid = -1;
    }
}

/* Commit the last swap: the parked predecessor is no longer needed. */
static void commit_parked(void)
{
    if (parked.coid != -1) {
        printf("shell: swap to v%u committed, retiring v%u (pid %d)\n",
               cur.version, parked.version, parked.pid);
        fflush(stdout);
        retire(&parked);
    }
}

static void commit_if_probation_over(void)
{
    if (parked.coid != -1 && now_ns() >= probation_end_ns) {
        commit_parked();
    }
}

/* The current core failed during probation: put the predecessor back. */
static int rollback(int err)
{
    uint64_t t0 = now_ns();
    struct core_ref failed = cur;

    ConnectDetach(failed.coid);
    kill(failed.pid, SIGKILL);  /* it may be alive but broken; make sure */

    if (core_call(parked.coid, CORE_OP_IMPORT, &parked_snapshot, NULL, 0) == -1) {
        fprintf(stderr, "shell: rollback failed, v%u rejected its own state: %s\n",
                parked.version, strerror(errno));
        retire(&parked);
        cur.coid = -1;
        return -1;
    }
    cur = parked;
    parked.coid = -1;

    printf("shell: ROLLBACK: core v%u (pid %d) failed (%s); v%u (pid %d) "
           "restored at value %llu in %.1f us\n",
           failed.version, failed.pid, strerror(err), cur.version, cur.pid,
           (unsigned long long)parked_snapshot.value, (now_ns() - t0) / 1000.0);
    fflush(stdout);
    return 0;
}

/* ---- read(): forward to the current core ----------------------------- */

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    /* Only plain reads are supported (no pread/readcond variants). */
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }

    /* No timer thread: probation expiry is checked lazily on each request.
     * The retire message this may send costs a few microseconds once. */
    commit_if_probation_over();

    if (cur.coid == -1) {
        return EAGAIN;              /* no core registered yet */
    }

    struct counter_rec rec;
    if (core_call(cur.coid, CORE_OP_READ, NULL, &rec, sizeof rec) == -1) {
        int err = errno;
        /* During probation a failure of the new core is answered by rolling
         * back and retrying once on the restored predecessor. */
        if (parked.coid == -1 || rollback(err) == -1 ||
            core_call(cur.coid, CORE_OP_READ, NULL, &rec, sizeof rec) == -1) {
            fprintf(stderr, "shell: core v%u (pid %d) read failed: %s\n",
                    cur.version, cur.pid, strerror(err));
            return EIO;
        }
    }

    /* Reply directly: rec lives on this stack frame, so it must be sent
     * before we return (returning an iov would reference freed memory). */
    size_t n = msg->i.nbytes < sizeof rec ? msg->i.nbytes : sizeof rec;
    MsgReply(ctp->rcvid, (long)n, &rec, n);
    return _RESMGR_NOREPLY;
}

/* ---- devctl(): core registration and the swap itself ------------------ */

static int do_swap(resmgr_context_t *ctp, const struct hotswap_register *reg)
{
    uint64_t t0 = now_ns();

    if (reg->fmt_min == 0 || reg->fmt_min > reg->fmt_max) {
        return EINVAL;
    }

    /* A new swap while the previous one is on probation commits it. */
    commit_parked();

    /* Use the kernel's view of the sender, not anything inside the message. */
    pid_t new_pid = ctp->info.pid;
    int new_coid = ConnectAttach(0, new_pid, reg->chid, _NTO_SIDE_CHANNEL, 0);
    if (new_coid == -1) {
        fprintf(stderr, "shell: cannot connect to core pid %d chid %d: %s\n",
                new_pid, reg->chid, strerror(errno));
        return errno;
    }

    /* 1. Take the state out of the old core: a full snapshot in its own
     *    newest format (kept for rollback), and the handover copy in a
     *    format the new core can import. */
    struct core_state snapshot = { .format = reg->fmt_max };
    struct core_state handover = { .format = reg->fmt_max };
    if (cur.coid != -1) {
        if (core_export(cur.coid, UINT16_MAX, &snapshot) == -1) {
            /* Old core is gone or broken: continue with fresh state so the
             * device keeps working, but say so loudly. */
            fprintf(stderr, "shell: export from core v%u failed (%s); "
                    "new core starts with empty state\n",
                    cur.version, strerror(errno));
            memset(&snapshot, 0, sizeof snapshot);
            snapshot.format = reg->fmt_max;
            handover = snapshot;
        } else if (snapshot.format > reg->fmt_max) {
            /* Downgrade: ask for an older format the new core understands. */
            if (core_export(cur.coid, reg->fmt_max, &handover) == -1 ||
                handover.format < reg->fmt_min) {
                int err = EPROTO;
                fprintf(stderr, "shell: v%u cannot write a state format v%u "
                        "reads (%u..%u); keeping v%u\n", cur.version,
                        reg->core_version, reg->fmt_min, reg->fmt_max, cur.version);
                core_call(cur.coid, CORE_OP_IMPORT, &snapshot, NULL, 0);
                ConnectDetach(new_coid);
                return err;
            }
        } else {
            handover = snapshot;
        }
    }
    uint64_t t1 = now_ns();

    /* 2. Give it to the new core. If that fails, keep the old core. */
    if (core_call(new_coid, CORE_OP_IMPORT, &handover, NULL, 0) == -1) {
        int err = errno;
        fprintf(stderr, "shell: import into new core v%u failed: %s; keeping v%u\n",
                reg->core_version, strerror(err), cur.version);
        ConnectDetach(new_coid);
        if (cur.coid != -1) {
            /* The old core stopped serving after export: give it back. */
            core_call(cur.coid, CORE_OP_IMPORT, &snapshot, NULL, 0);
        }
        return err;
    }
    uint64_t t2 = now_ns();

    /* 3. Switch. From here on, reads go to the new core. The old one is
     *    parked for the probation period (or retired now if there is none). */
    struct core_ref old = cur;
    cur.coid    = new_coid;
    cur.pid     = new_pid;
    cur.version = reg->core_version;

    if (old.coid == -1) {
        printf("shell: core v%u (pid %d) attached\n", cur.version, new_pid);
    } else {
        printf("shell: swapped v%u -> v%u (pid %d), value=%llu, state format "
               "%u -> %u, switch took %.1f us (export %.1f, import %.1f)\n",
               old.version, cur.version, new_pid,
               (unsigned long long)handover.value, snapshot.format, handover.format,
               (t2 - t0) / 1000.0, (t1 - t0) / 1000.0, (t2 - t1) / 1000.0);
        parked           = old;
        parked_snapshot  = snapshot;
        probation_end_ns = now_ns() + probation_ns;
        if (probation_ns == 0) {
            commit_parked();
        }
    }
    fflush(stdout);
    return EOK;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    /* Let the library handle the generic devctls (e.g. DCMD_ALL_GETFLAGS). */
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) {
        return status;
    }
    if (msg->i.dcmd != DCMD_HOTSWAP_REGISTER) {
        return ENOSYS;
    }
    if (ctp->info.msglen < (ssize_t)(sizeof(msg->i) + sizeof(struct hotswap_register))) {
        return EBADMSG;
    }

    /* Only a process running as the same user as the shell may become the
     * driver core; otherwise any local process could take over the device. */
    struct _client_info *cinfo = NULL;
    if (ConnectClientInfoExt(ctp->info.scoid, &cinfo, 0) == -1) {
        return errno;
    }
    uid_t euid = cinfo->cred.euid;
    ClientInfoExtFree(&cinfo);
    if (euid != geteuid()) {
        return EPERM;
    }

    struct hotswap_register reg;
    memcpy(&reg, _DEVCTL_DATA(msg->i), sizeof reg);

    status = do_swap(ctp, &reg);
    if (status != EOK) {
        return status;
    }
    memset(&msg->o, 0, sizeof msg->o);
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        if (opt == 't') {
            probation_ns = (uint64_t)strtoull(optarg, NULL, 10) * 1000000ull;
        } else {
            fprintf(stderr, "usage: %s [-t probation_ms]\n", argv[0]);
            return 2;
        }
    }

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) {
        perror("dispatch_create");
        return 1;
    }

    /* Function tables: start from the library defaults, override two. */
    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t      io_funcs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read   = io_read;
    io_funcs.devctl = io_devctl;

    /* The device node: a character device everyone may open. Access to the
     * swap itself is checked in io_devctl. */
    static iofunc_attr_t attr;
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 2048;

    if (resmgr_attach(dpp, &rattr, HOTSWAP_PATH, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        fprintf(stderr, "shell: cannot attach %s: %s (needs root or the "
                "pathspace ability)\n", HOTSWAP_PATH, strerror(errno));
        return 1;
    }

    printf("shell: serving %s (pid %d), probation %llu ms, waiting for a core\n",
           HOTSWAP_PATH, getpid(), (unsigned long long)(probation_ns / 1000000ull));
    fflush(stdout);

    /* Single-threaded receive loop: one request at a time, by design. */
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            perror("dispatch_block");
            return 1;
        }
        dispatch_handler(ctp);
    }
}
