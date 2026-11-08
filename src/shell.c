/*
 * shell.c - the stable half of a hot-swappable QNX driver.
 *
 * The shell is a QNX resource manager that owns /dev/counter. It holds no
 * driver logic: every read() is forwarded to the current "core" process and
 * the core's answer is passed back to the client.
 *
 * A new core version announces itself with DCMD_HOTSWAP_REGISTER. The shell
 * moves the state from the old core to the new one and switches over. The
 * shell is single-threaded, so while the swap runs, client requests wait in
 * the kernel on the shell's channel (SEND-blocked) and are served by the new
 * core as soon as the devctl handler returns. Clients never see an error and
 * keep their file descriptors.
 *
 * Usage: shell
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "hotswap_proto.h"

/* Connection to the core that currently serves reads; -1 = no core yet. */
static int      cur_coid = -1;
static pid_t    cur_pid;
static uint32_t cur_version;

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
    if (cur_coid == -1) {
        return EAGAIN;              /* no core registered yet */
    }

    struct counter_rec rec;
    if (core_call(cur_coid, CORE_OP_READ, NULL, &rec, sizeof rec) == -1) {
        fprintf(stderr, "shell: core v%u (pid %d) read failed: %s\n",
                cur_version, cur_pid, strerror(errno));
        return EIO;
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

    /* Use the kernel's view of the sender, not anything inside the message. */
    pid_t new_pid = ctp->info.pid;

    int new_coid = ConnectAttach(0, new_pid, reg->chid, _NTO_SIDE_CHANNEL, 0);
    if (new_coid == -1) {
        fprintf(stderr, "shell: cannot connect to core pid %d chid %d: %s\n",
                new_pid, reg->chid, strerror(errno));
        return errno;
    }

    /* 1. Take the state out of the old core (if there is one). */
    struct core_state state = { .format = CORE_STATE_FORMAT, .value = 0 };
    int have_state = 0;
    if (cur_coid != -1) {
        if (core_call(cur_coid, CORE_OP_EXPORT, NULL, &state, sizeof state) == 0) {
            have_state = 1;
        } else {
            /* Old core is gone or broken: continue with fresh state so the
             * device keeps working, but say so loudly. */
            fprintf(stderr, "shell: export from core v%u failed (%s); "
                    "new core starts with empty state\n",
                    cur_version, strerror(errno));
            state.value = 0;
        }
    }
    uint64_t t1 = now_ns();

    /* 2. Give it to the new core. If that fails, keep the old core. */
    if (core_call(new_coid, CORE_OP_IMPORT, &state, NULL, 0) == -1) {
        int err = errno;
        fprintf(stderr, "shell: import into new core failed: %s; keeping v%u\n",
                strerror(err), cur_version);
        ConnectDetach(new_coid);
        if (have_state) {
            /* The old core stopped serving after export: hand the state back. */
            core_call(cur_coid, CORE_OP_IMPORT, &state, NULL, 0);
        }
        return err;
    }
    uint64_t t2 = now_ns();

    /* 3. Switch. From here on, reads go to the new core. */
    int      old_coid    = cur_coid;
    uint32_t old_version = cur_version;
    cur_coid    = new_coid;
    cur_pid     = new_pid;
    cur_version = reg->core_version;

    /* 4. Retire the old core. */
    if (old_coid != -1) {
        core_call(old_coid, CORE_OP_QUIT, NULL, NULL, 0);
        ConnectDetach(old_coid);
    }
    uint64_t t3 = now_ns();

    if (old_coid == -1) {
        printf("shell: core v%u (pid %d) attached\n", cur_version, new_pid);
    } else {
        printf("shell: swapped v%u -> v%u (pid %d), value=%llu, "
               "switch took %.1f us (export %.1f, import %.1f, retire %.1f)\n",
               old_version, cur_version, new_pid,
               (unsigned long long)state.value,
               (t3 - t0) / 1000.0, (t1 - t0) / 1000.0,
               (t2 - t1) / 1000.0, (t3 - t2) / 1000.0);
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

int main(void)
{
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

    printf("shell: serving %s (pid %d), waiting for a core\n", HOTSWAP_PATH, getpid());
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
