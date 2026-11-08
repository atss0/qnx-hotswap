/*
 * hotswap_proto.h - messages shared by the shell, the core and the client.
 *
 * Process layout:
 *
 *   client --read()--> shell (/dev/counter) --MsgSend--> core vN
 *
 * The shell owns the path /dev/counter. Clients open it once and keep the
 * same file descriptor for their whole life. The shell forwards each read to
 * the current core over a private QNX channel.
 *
 * Upgrade: a new core starts, creates its channel and sends
 * DCMD_HOTSWAP_REGISTER to the shell. The shell then (all inside one devctl
 * handler, so client reads simply wait on the shell's channel meanwhile):
 *   1. connects to the new core,
 *   2. asks the old core for its state (CORE_OP_EXPORT),
 *   3. gives that state to the new core (CORE_OP_IMPORT),
 *   4. makes the new core current and tells the old one to exit (CORE_OP_QUIT).
 */
#ifndef HOTSWAP_PROTO_H
#define HOTSWAP_PROTO_H

#include <stdint.h>
#include <devctl.h>
#include <sys/iomsg.h>

#define HOTSWAP_PATH "/dev/counter"

/* ---- client <-> shell ------------------------------------------------- */

/* One read() of /dev/counter returns exactly this record. */
struct counter_rec {
    uint64_t value;         /* counter value; +1 on every read */
    uint32_t core_version;  /* version of the core that served the read */
    uint32_t reserved;
};

/* ---- core -> shell: registration ---------------------------------------- */

/* Sent by a newly started core: "connect to my channel <chid>". The shell
 * takes the pid from the kernel (the sender's real pid), not from here. */
struct hotswap_register {
    int32_t  chid;
    uint32_t core_version;
};

#define DCMD_HOTSWAP_REGISTER __DIOT(_DCMD_MISC, 0x48, struct hotswap_register)

/* ---- shell <-> core ----------------------------------------------------- */

/* Everything a core must hand to its successor. format lets a new core
 * reject (or convert) state written by an incompatible older version. */
#define CORE_STATE_FORMAT 1
struct core_state {
    uint32_t format;
    uint32_t reserved;
    uint64_t value;
};

/* Message type for the shell->core protocol. Chosen above the range used by
 * QNX I/O messages so it can never be mistaken for one. */
#define CORE_MSG_TYPE (_IO_MAX + 0x100)

enum core_op {
    CORE_OP_READ = 1,   /* reply: struct counter_rec */
    CORE_OP_EXPORT,     /* reply: struct core_state; core stops serving */
    CORE_OP_IMPORT,     /* msg carries struct core_state; reply: empty */
    CORE_OP_QUIT,       /* reply: empty; core exits */
};

struct core_msg {
    uint16_t type;      /* CORE_MSG_TYPE */
    uint16_t op;        /* enum core_op */
    uint32_t reserved;
    struct core_state state;   /* used by CORE_OP_IMPORT only */
};

#endif /* HOTSWAP_PROTO_H */
