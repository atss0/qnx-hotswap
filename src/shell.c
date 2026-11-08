/*
 * shell.c - the stable half of a hot-swappable QNX driver.
 *
 * The shell is a QNX resource manager that owns a device path such as
 * /dev/counter or /dev/accel. It holds no driver logic and does not know the
 * driver's record or state layout: every read(), write() and driver-specific
 * devctl() is forwarded to the current "core" process and the core's answer
 * is passed back to the client as is.
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
 * during that time, the old core is restored and the failed request retried
 * there, so the client still sees no error. During probation the new core's
 * state is checkpointed after every request, in a format the old core can
 * read, so the old core resumes from the last completed request and not from
 * the moment of the handover. After the probation period the swap is
 * committed and the old core exits. A new registration during probation is
 * refused (EBUSY): one upgrade at a time.
 *
 * Failure detection:
 *   - crash: the kernel sends a _PULSE_CODE_COIDDEATH pulse as soon as a
 *     core dies, even when nobody is reading, and a pending MsgSend fails;
 *   - hang: every MsgSend to a core has a kernel timeout (TimerTimeout). A
 *     core that does not answer in time is killed.
 *
 * Recovery after probation: if the only core is lost, reads return EAGAIN
 * (client descriptors stay valid). The shell keeps a periodic checkpoint of
 * the core's state (every -c ms); the next core that registers, started by
 * whoever supervises the system, continues from it.
 *
 * Usage: shell [-n path] [-t probation_ms] [-r read_timeout_us]
 *              [-x control_timeout_ms] [-c checkpoint_ms]
 *   -n  device path to own (default /dev/counter)
 *   -t  probation period (default 2000; 0 = retire the old core at once)
 *   -r  a read/write the core has not answered after this long means the
 *       core is hung (default 5000). Keep it above the driver's worst case:
 *       a false alarm kills a healthy core.
 *   -x  the same for export, import, checkpoint and quit (default 50)
 *   -c  checkpoint period, also the probation check period (default 100)
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

/* Importing into a core with no state may include hardware calibration. */
#define FRESH_IMPORT_TIMEOUT_NS (2000ull * 1000000ull)

/* Priority of the periodic work: above typical clients, so a checkpoint is
 * finished quickly instead of being stretched out by them. */
#define TICK_PRIO 55

/* A connection to one core process. coid == -1 means "none". */
struct core_ref {
    int      coid;
    pid_t    pid;
    uint32_t version;
    uint16_t fmt_min;
    uint16_t fmt_max;
};

static struct core_ref   cur    = { .coid = -1 };  /* serves requests */
static struct core_ref   parked = { .coid = -1 };  /* predecessor on probation */
static struct core_state parked_snapshot;          /* its own state at handover */

static struct core_state ckpt;              /* latest checkpoint of cur */
static int               ckpt_valid;
static int               ckpt_for_parked;   /* taken during this probation */

static uint64_t probation_end_ns;
static uint64_t probation_ns       = 2000ull * 1000000ull;
static uint64_t read_timeout_ns    = 5000ull * 1000ull;
static uint64_t control_timeout_ns = 50ull * 1000000ull;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Errors that mean "the core process is gone or stuck", as opposed to the
 * core answering that it could not do something. */
static int is_core_failure(int err)
{
    return err == ESRCH || err == EBADF || err == ETIMEDOUT || err == EINTR;
}

/* Send one request to a core and wait for its reply, for at most timeout_ns.
 * Returns the core's reply status (for CORE_OP_READ: the record size), or -1
 * with errno set (ETIMEDOUT if the core did not answer). */
static long core_call(int coid, enum core_op op, const struct core_state *in,
                      void *reply, size_t reply_len, uint64_t timeout_ns)
{
    static struct core_msg m;   /* single-threaded: one request at a time */
    size_t len = CORE_MSG_HDR_LEN;

    m.type = CORE_MSG_TYPE;
    m.op   = (uint16_t)op;
    if (in) {
        /* Copy and send only the bytes in use. */
        memcpy(&m.state, in, offsetof(struct core_state, data) + in->size);
        len = CORE_MSG_STATE_HDR_LEN + in->size;
    }
    /* The timeout applies to the next kernel call only. It covers both the
     * core not receiving (SEND-blocked) and not replying (REPLY-blocked). */
    uint64_t ns = timeout_ns;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &ns, NULL) == -1) {
        return -1;
    }
    return MsgSend(coid, &m, len, reply, reply_len);
}

/* Ask a core for its state in the newest format it can write that is not
 * newer than 'wanted'. */
static int core_export(int coid, enum core_op op, uint32_t wanted, struct core_state *out)
{
    struct core_state req = { .format = wanted, .size = 0 };
    return (int)core_call(coid, op, &req, out, sizeof *out, control_timeout_ns);
}

static int core_import(int coid, const struct core_state *st)
{
    return (int)core_call(coid, CORE_OP_IMPORT, st, NULL, 0,
                          st->size == 0 ? FRESH_IMPORT_TIMEOUT_NS : control_timeout_ns);
}

/* Remove a failed core: kill it if it is still there, drop the connection.
 * The kernel is asked who is really at the other end of the connection first,
 * so a recycled pid can never get the wrong process killed. */
static void stop_core(struct core_ref *c)
{
    struct _server_info si;

    if (c->coid == -1) {
        return;
    }
    if (ConnectServerInfo(0, c->coid, &si) == c->coid &&
        !(si.flags & _NTO_COF_DEAD) && si.pid == c->pid) {
        kill(c->pid, SIGKILL);
    }
    ConnectDetach(c->coid);
    c->coid = -1;
}

/* Tell a core to exit and drop our connection to it. */
static void retire(struct core_ref *c)
{
    if (c->coid == -1) {
        return;
    }
    if (core_call(c->coid, CORE_OP_QUIT, NULL, NULL, 0, control_timeout_ns) == -1) {
        stop_core(c);
        return;
    }
    ConnectDetach(c->coid);
    c->coid = -1;
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

/* The current core failed during probation: put the predecessor back. It
 * resumes from the newest checkpoint it can read, or else from the snapshot
 * of its own state taken at the handover. */
static int rollback(int err)
{
    uint64_t t0 = now_ns();
    struct core_ref failed = cur;

    stop_core(&cur);    /* it may be alive but broken; make sure */

    const struct core_state *st = &parked_snapshot;
    const char *from = "its handover snapshot";
    if (ckpt_for_parked && ckpt.format >= parked.fmt_min && ckpt.format <= parked.fmt_max) {
        st = &ckpt;
        from = "the last checkpoint";
    }

    if (core_import(parked.coid, st) == -1) {
        fprintf(stderr, "shell: rollback failed, v%u rejected the state: %s\n",
                parked.version, strerror(errno));
        stop_core(&parked);
        return -1;
    }
    cur = parked;
    parked.coid = -1;
    ckpt_for_parked = 0;

    printf("shell: ROLLBACK: core v%u (pid %d) failed (%s); v%u (pid %d) "
           "restored from %s in %.1f us\n",
           failed.version, failed.pid, strerror(err), cur.version, cur.pid,
           from, (now_ns() - t0) / 1000.0);
    fflush(stdout);
    return 0;
}

/* The current core failed and there is no predecessor to go back to. The
 * device stays open but has no service until a new core registers. */
static void core_lost(int err)
{
    fprintf(stderr, "shell: core v%u (pid %d) lost (%s); waiting for a new "
            "core, %s\n", cur.version, cur.pid, strerror(err),
            ckpt_valid ? "state checkpoint kept" : "no checkpoint");
    stop_core(&cur);
}

/* A core failed: go back to the predecessor if there is one. */
static void handle_core_failure(int err)
{
    if (parked.coid == -1 || rollback(err) == -1) {
        core_lost(err);
    }
}

/* Save the current core's state. During probation it is requested in a
 * format the parked core can read, for a rollback. */
static void take_checkpoint(void)
{
    static struct core_state tmp;

    if (cur.coid == -1) {
        return;
    }
    uint32_t wanted = parked.coid != -1 ? parked.fmt_max : UINT16_MAX;
    if (core_export(cur.coid, CORE_OP_CHECKPOINT, wanted, &tmp) == -1) {
        int err = errno;
        if (is_core_failure(err)) {
            handle_core_failure(err);
        }
        return;     /* otherwise: this core cannot write that format */
    }
    ckpt = tmp;
    ckpt_valid = 1;
    ckpt_for_parked = parked.coid != -1;
}

/* Is this connection one whose server process has died? A connection id can
 * be reused, so old death pulses are checked against the kernel's view. */
static int coid_is_dead(int coid)
{
    struct _server_info si;
    return ConnectServerInfo(0, coid, &si) == coid && (si.flags & _NTO_COF_DEAD);
}

/* ---- kernel and timer pulses ------------------------------------------ */

/* The kernel reports that the process behind one of our connections died. */
static int on_coid_death(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    (void)code; (void)flags; (void)handle;
    int coid = ctp->msg->pulse.value.sival_int;

    if (coid == -1 || !coid_is_dead(coid)) {
        return 0;
    }
    if (coid == cur.coid) {
        handle_core_failure(ESRCH);
    } else if (coid == parked.coid) {
        fprintf(stderr, "shell: parked core v%u (pid %d) died; no rollback "
                "possible for this swap\n", parked.version, parked.pid);
        stop_core(&parked);
    }
    return 0;
}

/* Periodic housekeeping: end the probation period and checkpoint the core. */
static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    (void)ctp; (void)code; (void)flags; (void)handle;

    if (parked.coid != -1 && now_ns() >= probation_end_ns) {
        if (cur.coid != -1 && coid_is_dead(cur.coid)) {
            handle_core_failure(ESRCH);     /* never commit a dead core */
        } else {
            commit_parked();
        }
    }
    take_checkpoint();
    return 0;
}

/* ---- client requests: forward to the current core --------------------- */

/* Forward a request. If the core fails it is replaced (rollback) and the
 * request retried there; a retry cannot apply a request twice because the
 * failed core's state is discarded. Returns the core's status, or -1 with
 * errno set: EAGAIN = no core right now, try again later. */
static long forward(enum core_op op, const struct core_state *in,
                    void *reply, size_t reply_len, uint64_t timeout_ns)
{
    if (cur.coid == -1) {
        errno = EAGAIN;
        return -1;
    }
    long r = core_call(cur.coid, op, in, reply, reply_len, timeout_ns);
    if (r != -1) {
        return r;
    }
    int err = errno;

    /* During probation any failure, even a reported one, sends us back. */
    if (parked.coid != -1 && rollback(err) == 0) {
        r = core_call(cur.coid, op, in, reply, reply_len, timeout_ns);
        if (r != -1) {
            return r;
        }
        err = errno;
    }
    if (cur.coid != -1 && parked.coid == -1 && is_core_failure(err)) {
        core_lost(err);
    }
    if (cur.coid == -1) {
        err = EAGAIN;
    }
    errno = err;
    return -1;
}

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

    /* The record is opaque to the shell: the core's reply status is its
     * size, and the bytes are passed on unchanged. */
    uint8_t rec[CORE_MAX_REPLY];
    long len = forward(CORE_OP_READ, NULL, rec, sizeof rec, read_timeout_ns);
    if (len == -1) {
        return errno == EAGAIN ? EAGAIN : EIO;
    }
    if (len > (long)sizeof rec) {
        len = sizeof rec;
    }

    /* Reply directly: rec lives on this stack frame, so it must be sent
     * before we return (returning an iov would reference freed memory). */
    size_t n = msg->i.nbytes < (size_t)len ? msg->i.nbytes : (size_t)len;
    MsgReply(ctp->rcvid, (long)n, rec, n);

    /* The client is already on its way. Now, while on probation, save the
     * state this request produced so a rollback loses nothing. */
    if (parked.coid != -1) {
        take_checkpoint();
    }
    return _RESMGR_NOREPLY;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    if (msg->i.nbytes > CORE_STATE_MAX) {
        return EMSGSIZE;
    }

    static struct core_state req;
    req.format = 0;
    req.size   = (uint32_t)msg->i.nbytes;
    if (req.size > 0 && resmgr_msgread(ctp, req.data, req.size, sizeof msg->i) != (ssize_t)req.size) {
        return EBADMSG;
    }

    long n = forward(CORE_OP_WRITE, &req, NULL, 0, read_timeout_ns);
    if (n == -1) {
        return errno;
    }
    _IO_SET_WRITE_NBYTES(ctp, n);
    if (parked.coid != -1) {
        take_checkpoint();
    }
    return EOK;
}

/* ---- devctl(): core registration and the swap itself ------------------ */

static int do_swap(resmgr_context_t *ctp, const struct hotswap_register *reg)
{
    uint64_t t0 = now_ns();

    if (reg->fmt_min == 0 || reg->fmt_min > reg->fmt_max) {
        return EINVAL;
    }

    /* One upgrade at a time: the previous one must be committed or rolled
     * back before the next can start. */
    if (parked.coid != -1) {
        return EBUSY;
    }

    /* Use the kernel's view of the sender, not anything inside the message. */
    struct core_ref neu = {
        .pid     = ctp->info.pid,
        .version = reg->core_version,
        .fmt_min = reg->fmt_min,
        .fmt_max = reg->fmt_max,
    };
    neu.coid = ConnectAttach(0, neu.pid, reg->chid, _NTO_SIDE_CHANNEL, 0);
    if (neu.coid == -1) {
        fprintf(stderr, "shell: cannot connect to core pid %d chid %d: %s\n",
                neu.pid, reg->chid, strerror(errno));
        return errno;
    }

    /* 1. Take the state out of the old core: a full snapshot in its own
     *    newest format (kept for rollback), and the handover copy in a
     *    format the new core can import. */
    static struct core_state snapshot, handover;
    memset(&snapshot, 0, offsetof(struct core_state, data));
    memset(&handover, 0, offsetof(struct core_state, data));
    snapshot.format = handover.format = reg->fmt_max;

    if (cur.coid != -1 &&
        core_export(cur.coid, CORE_OP_EXPORT, UINT16_MAX, &snapshot) == -1) {
        int err = errno;
        if (!is_core_failure(err)) {
            /* Alive, but it refuses to give up its state: no swap. */
            fprintf(stderr, "shell: core v%u refused to export its state: %s\n",
                    cur.version, strerror(err));
            ConnectDetach(neu.coid);
            return EPROTO;
        }
        /* Dead or hung: continue as if it had been lost a moment ago. */
        core_lost(err);
    }

    int recovered = 0;
    if (cur.coid != -1) {
        if (snapshot.format > reg->fmt_max) {
            /* Downgrade: ask for an older format the new core understands. */
            if (core_export(cur.coid, CORE_OP_EXPORT, reg->fmt_max, &handover) == -1 ||
                handover.format < reg->fmt_min) {
                fprintf(stderr, "shell: v%u cannot write a state format v%u "
                        "reads (%u..%u); keeping v%u\n", cur.version,
                        reg->core_version, reg->fmt_min, reg->fmt_max, cur.version);
                if (core_import(cur.coid, &snapshot) == -1) {
                    core_lost(errno);
                }
                ConnectDetach(neu.coid);
                return EPROTO;
            }
        } else {
            handover = snapshot;
        }
    } else if (ckpt_valid && ckpt.format >= reg->fmt_min && ckpt.format <= reg->fmt_max) {
        /* No predecessor to ask: continue from the last checkpoint. */
        handover = ckpt;
        recovered = 1;
    } else if (ckpt_valid) {
        fprintf(stderr, "shell: checkpoint has format %u, core v%u reads "
                "%u..%u: starting with empty state\n", ckpt.format,
                reg->core_version, reg->fmt_min, reg->fmt_max);
    }
    uint64_t t1 = now_ns();

    /* 2. Give it to the new core. If that fails, keep the old core. */
    if (core_import(neu.coid, &handover) == -1) {
        int err = errno;
        fprintf(stderr, "shell: import into new core v%u failed: %s; keeping v%u\n",
                reg->core_version, strerror(err), cur.version);
        if (is_core_failure(err)) {
            stop_core(&neu);        /* it hangs: do not leave it behind */
        } else {
            ConnectDetach(neu.coid);
        }
        /* The old core stopped serving after export: give it back. */
        if (cur.coid != -1 && core_import(cur.coid, &snapshot) == -1) {
            core_lost(errno);
        }
        return is_core_failure(err) ? EIO : err;
    }
    uint64_t t2 = now_ns();

    /* 3. Switch. From here on, requests go to the new core. The old one is
     *    parked for the probation period (or retired now if there is none). */
    struct core_ref old = cur;
    cur = neu;

    /* The handover state is the first checkpoint of the new core. */
    ckpt_for_parked = 0;
    ckpt_valid = handover.size > 0;
    if (ckpt_valid) {
        ckpt = handover;
    }

    if (old.coid == -1) {
        if (recovered) {
            printf("shell: core v%u (pid %d) attached, recovered format %u "
                   "state from checkpoint in %.1f us\n", cur.version, cur.pid,
                   handover.format, (t2 - t0) / 1000.0);
        } else {
            printf("shell: core v%u (pid %d) attached\n", cur.version, cur.pid);
        }
    } else {
        printf("shell: swapped v%u -> v%u (pid %d), handed over %u bytes of "
               "format %u state (v%u's own: format %u), switch took %.1f us "
               "(export %.1f, import %.1f)\n",
               old.version, cur.version, cur.pid, handover.size, handover.format,
               old.version, snapshot.format,
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

/* A driver-specific devctl: forwarded unexamined, like a read. */
static int forward_devctl(resmgr_context_t *ctp, io_devctl_t *msg)
{
    if (msg->i.nbytes > CORE_STATE_MAX) {
        return EMSGSIZE;
    }

    static struct core_state req;
    size_t avail = (size_t)ctp->info.msglen > sizeof msg->i
                 ? (size_t)ctp->info.msglen - sizeof msg->i : 0;
    size_t in = avail < msg->i.nbytes ? avail : msg->i.nbytes;
    req.format = (uint32_t)msg->i.dcmd;
    req.size   = (uint32_t)in;
    memcpy(req.data, _DEVCTL_DATA(msg->i), in);

    /* Keep the size first: 'msg' holds the request until it is overwritten. */
    size_t out_max = msg->i.nbytes;
    static uint8_t out[CORE_STATE_MAX];
    long n = forward(CORE_OP_CTL, &req, out, sizeof out, control_timeout_ns);
    if (n == -1) {
        return errno;
    }
    if ((size_t)n > out_max) {
        n = (long)out_max;
    }

    memset(&msg->o, 0, sizeof msg->o);
    msg->o.nbytes = (uint32_t)n;
    memcpy(_DEVCTL_DATA(msg->o), out, (size_t)n);
    if (parked.coid != -1) {
        take_checkpoint();
    }
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o + (size_t)n);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    /* Let the library handle the generic devctls (e.g. DCMD_ALL_GETFLAGS). */
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) {
        return status;
    }
    if (msg->i.dcmd != DCMD_HOTSWAP_REGISTER) {
        return forward_devctl(ctp, msg);
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
    const char *path = "/dev/counter";
    long checkpoint_ms = 100;
    int opt;
    while ((opt = getopt(argc, argv, "n:t:r:x:c:")) != -1) {
        switch (opt) {
        case 'n': path = optarg; break;
        case 't': probation_ns = strtoull(optarg, NULL, 10) * 1000000ull; break;
        case 'r': read_timeout_ns = strtoull(optarg, NULL, 10) * 1000ull; break;
        case 'x': control_timeout_ns = strtoull(optarg, NULL, 10) * 1000000ull; break;
        case 'c': checkpoint_ms = atol(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-n path] [-t probation_ms] "
                    "[-r read_timeout_us] [-x control_timeout_ms] "
                    "[-c checkpoint_ms]\n", argv[0]);
            return 2;
        }
    }
    if (checkpoint_ms < 1 || read_timeout_ns == 0 || control_timeout_ns == 0) {
        fprintf(stderr, "shell: timeouts and the checkpoint period must be > 0\n");
        return 2;
    }

    /* We create the channel ourselves so it can carry the death notification
     * for the connections to the cores: _NTO_CHF_COID_DISCONNECT. */
    int chid = ChannelCreate(_NTO_CHF_COID_DISCONNECT | _NTO_CHF_DISCONNECT |
                             _NTO_CHF_UNBLOCK);
    if (chid == -1) {
        perror("ChannelCreate");
        return 1;
    }
    dispatch_t *dpp = dispatch_create_channel(chid, 0);
    if (dpp == NULL) {
        perror("dispatch_create_channel");
        return 1;
    }

    /* Function tables: start from the library defaults, override three. */
    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t      io_funcs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read   = io_read;
    io_funcs.write  = io_write;
    io_funcs.devctl = io_devctl;

    /* The device node: everyone may read, only the owner may write. The
     * swap needs a read/write descriptor, and io_devctl checks the user. */
    static iofunc_attr_t attr;
    iofunc_attr_init(&attr, S_IFCHR | 0644, NULL, NULL);
    attr.uid = geteuid();
    attr.gid = getegid();

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 2048;

    if (resmgr_attach(dpp, &rattr, path, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        fprintf(stderr, "shell: cannot attach %s: %s (needs root or the "
                "pathspace ability)\n", path, strerror(errno));
        return 1;
    }

    if (pulse_attach(dpp, 0, _PULSE_CODE_COIDDEATH, on_coid_death, NULL) == -1) {
        perror("pulse_attach(COIDDEATH)");
        return 1;
    }

    /* A periodic timer delivers a pulse to our own receive loop, so the
     * housekeeping runs in the same single thread as everything else and
     * needs no locking. */
    int tick_code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
    int tick_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (tick_code == -1 || tick_coid == -1) {
        perror("shell: tick pulse");
        return 1;
    }
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, tick_coid, TICK_PRIO, tick_code, 0);
    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        perror("timer_create");
        return 1;
    }
    struct itimerspec its = {
        .it_value    = { checkpoint_ms / 1000, (checkpoint_ms % 1000) * 1000000 },
        .it_interval = { checkpoint_ms / 1000, (checkpoint_ms % 1000) * 1000000 },
    };
    timer_settime(timer, 0, &its, NULL);

    printf("shell: serving %s (pid %d), probation %llu ms, read timeout %llu us, "
           "checkpoint every %ld ms, waiting for a core\n",
           path, getpid(), (unsigned long long)(probation_ns / 1000000ull),
           (unsigned long long)(read_timeout_ns / 1000ull), checkpoint_ms);
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
