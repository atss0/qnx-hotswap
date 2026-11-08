/*
 * hotswap_proto.h - messages between the shell, the cores and clients.
 *
 * Process layout:
 *
 *   client --read()--> shell (/dev/<name>) --MsgSend--> core vN
 *
 * The shell owns a path such as /dev/counter or /dev/accel. Clients open it
 * once and keep the same file descriptor for their whole life. The shell
 * forwards each read to the current core over a private QNX channel and
 * passes the reply back unchanged: it knows nothing about the driver.
 *
 * Upgrade: a new core starts, creates its channel and sends
 * DCMD_HOTSWAP_REGISTER to the shell. The shell then (inside one devctl
 * handler, so client reads simply wait on the shell's channel meanwhile):
 *   1. connects to the new core,
 *   2. asks the old core for its state (CORE_OP_EXPORT) in a format the new
 *      core can read,
 *   3. gives that state to the new core (CORE_OP_IMPORT),
 *   4. makes the new core current and parks the old one for a probation
 *      period, after which it is told to exit (CORE_OP_QUIT).
 *
 * The shell also takes periodic checkpoints of the current core's state
 * (CORE_OP_CHECKPOINT), so a core that dies later can be replaced without
 * starting from nothing.
 *
 * The state is an opaque blob to the shell: only its format number matters.
 */
#ifndef HOTSWAP_PROTO_H
#define HOTSWAP_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <devctl.h>
#include <sys/iomsg.h>

/* Largest record a core may return for one read. */
#define CORE_MAX_REPLY 256

/* ---- core -> shell: registration ---------------------------------------- */

/* Sent by a newly started core: "connect to my channel <chid>; I can import
 * state formats fmt_min..fmt_max". The shell takes the pid from the kernel
 * (the sender's real pid), not from here. */
struct hotswap_register {
    int32_t  chid;
    uint32_t core_version;
    uint16_t fmt_min;
    uint16_t fmt_max;
    uint32_t reserved;
};

#define DCMD_HOTSWAP_REGISTER __DIOT(_DCMD_MISC, 0x48, struct hotswap_register)

/* ---- shell <-> core ----------------------------------------------------- */

/* Driver state in transit. Each driver defines what its formats contain;
 * size == 0 means "no state": the driver starts fresh (first start, or the
 * predecessor could not be asked). */
#define CORE_STATE_MAX 512
struct core_state {
    uint32_t format;
    uint32_t size;                  /* bytes used in data[] */
    uint8_t  data[CORE_STATE_MAX];
};

/* Message type for the shell->core protocol. Chosen above the range used by
 * QNX I/O messages so it can never be mistaken for one. */
#define CORE_MSG_TYPE (_IO_MAX + 0x100)

enum core_op {
    CORE_OP_READ = 1,   /* reply: one driver record; MsgSend returns its size */
    CORE_OP_EXPORT,     /* msg: state.format = newest wanted format;
                           reply: struct core_state; core stops serving */
    CORE_OP_IMPORT,     /* msg carries struct core_state; reply: empty */
    CORE_OP_QUIT,       /* reply: empty; core exits */
    CORE_OP_CHECKPOINT, /* like EXPORT, but the core keeps serving */
    CORE_OP_WRITE,      /* msg: state.size bytes in state.data;
                           MsgSend returns the number of bytes taken */
    CORE_OP_CTL,        /* driver-specific devctl: state.format = dcmd,
                           state.size input bytes in state.data;
                           reply: output bytes, MsgSend returns their number */
};

struct core_msg {
    uint16_t type;      /* CORE_MSG_TYPE */
    uint16_t op;        /* enum core_op */
    uint32_t reserved;
    struct core_state state;   /* used by EXPORT (format only), IMPORT, ... */
};

/* Only the part of a message that an operation needs is sent, so the common
 * case (CORE_OP_READ) moves 8 bytes instead of more than 500. */
#define CORE_MSG_HDR_LEN        offsetof(struct core_msg, state)
#define CORE_MSG_STATE_HDR_LEN  (CORE_MSG_HDR_LEN + offsetof(struct core_state, data))

#endif /* HOTSWAP_PROTO_H */
