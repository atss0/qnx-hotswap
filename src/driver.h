/*
 * driver.h - what a driver must provide to be hot-swappable.
 *
 * A driver is written once, as three callbacks plus some constants, and can
 * then be linked into two different hosts:
 *
 *   core_main.c  - a swappable core behind the shell (the hot-swap design)
 *   mono_main.c  - a conventional resource manager that owns the path
 *                  itself (the stop-and-restart baseline)
 *
 * Because both hosts run exactly the same driver code, any difference seen
 * by clients comes from the hosting model alone.
 *
 * DRV_VERSION is set by the Makefile for each build (v1, v2, ...).
 */
#ifndef DRIVER_H
#define DRIVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "hotswap_proto.h"

struct driver {
    const char *path;       /* default device path, e.g. "/dev/counter" */
    uint32_t    version;    /* DRV_VERSION */
    uint16_t    fmt_min;    /* oldest state format this build can import */
    uint16_t    fmt_max;    /* newest state format this build writes */

    /* Take over state from a predecessor, converting older formats.
     * st->size == 0 means there is no predecessor state: start fresh (this
     * may take time, e.g. sensor calibration). Return 0, or -1 to refuse. */
    int (*import_state)(const struct core_state *st);

    /* Write the current state in the newest format <= wanted.
     * Return 0, or -1 if the oldest format we can write is still too new. */
    int (*export_state)(uint32_t wanted, struct core_state *st);

    /* Produce one record for a client read into buf (at most max bytes).
     * Return the record size. */
    ssize_t (*read)(void *buf, size_t max);

    /* Optional (may be NULL): take a client's write(). Return the number of
     * bytes taken, or -1. */
    ssize_t (*write)(const void *buf, size_t len);

    /* Optional (may be NULL): a driver-specific devctl. data holds 'in'
     * input bytes and has room for 'max' output bytes. Return the number of
     * output bytes, or -1. */
    ssize_t (*control)(uint32_t dcmd, void *data, size_t in, size_t max);
};

/* Each driver source file defines exactly one of these. */
extern const struct driver drv;

#endif /* DRIVER_H */
