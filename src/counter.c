/*
 * counter.c - a toy driver: each read returns the next counter value.
 *
 * Its state is tiny, which makes state loss easy to see: after a restart
 * the counter starts again from 1.
 *
 * State formats:
 *   1 (v1): value
 *   2 (v2): value, generation (number of handovers survived)
 * v2 converts format 1 on upgrade and writes format 1 on downgrade.
 */
#include <stdio.h>
#include <string.h>

#include "counter.h"
#include "driver.h"

#ifndef DRV_VERSION
#define DRV_VERSION 1
#endif

#if DRV_VERSION >= 2
#define FMT_MAX 2
#else
#define FMT_MAX 1
#endif

struct counter_state_f1 { uint64_t value; };
struct counter_state_f2 { uint64_t value; uint64_t generation; };

static uint64_t counter_value;
static uint64_t generation;

static int counter_import(const struct core_state *st)
{
    if (st->size == 0) {            /* fresh start */
        counter_value = 0;
        generation = 0;
        return 0;
    }
    if (st->format == 1 && st->size >= sizeof(struct counter_state_f1)) {
        const struct counter_state_f1 *s = (const void *)st->data;
        counter_value = s->value;
        generation = 0;             /* format 1 has none: start counting now */
    } else if (st->format == 2 && FMT_MAX >= 2 &&
               st->size >= sizeof(struct counter_state_f2)) {
        const struct counter_state_f2 *s = (const void *)st->data;
        counter_value = s->value;
        generation = s->generation;
    } else {
        return -1;
    }
#if FMT_MAX >= 2
    generation++;
    printf("counter v%d: imported format %u state (value %llu, generation %llu)%s\n",
           DRV_VERSION, st->format, (unsigned long long)counter_value,
           (unsigned long long)generation,
           st->format < FMT_MAX ? ", converted from older format" : "");
    /* No fflush here: the host flushes after replying (part of a swap). */
#endif
    return 0;
}

static int counter_export(uint32_t wanted, struct core_state *st)
{
    if (wanted < 1) {
        return -1;
    }
    memset(st, 0, sizeof *st);
    st->format = wanted < FMT_MAX ? wanted : FMT_MAX;
    if (st->format >= 2) {
        struct counter_state_f2 s = { .value = counter_value, .generation = generation };
        memcpy(st->data, &s, sizeof s);
        st->size = sizeof s;
    } else {
        struct counter_state_f1 s = { .value = counter_value };
        memcpy(st->data, &s, sizeof s);
        st->size = sizeof s;
    }
    return 0;
}

static ssize_t counter_read(void *buf, size_t max)
{
    struct counter_rec rec = {
        .value = ++counter_value,
        .core_version = DRV_VERSION,
    };
    size_t n = max < sizeof rec ? max : sizeof rec;
    memcpy(buf, &rec, n);
    return (ssize_t)n;
}

/* write(): set the counter to the uint64_t that was written. */
static ssize_t counter_write(const void *buf, size_t len)
{
    if (len != sizeof(uint64_t)) {
        return -1;
    }
    memcpy(&counter_value, buf, sizeof counter_value);
    return (ssize_t)len;
}

static ssize_t counter_control(uint32_t dcmd, void *data, size_t in, size_t max)
{
    uint64_t n;
    if (dcmd != (uint32_t)DCMD_COUNTER_ADD || in < sizeof n || max < sizeof n) {
        return -1;
    }
    memcpy(&n, data, sizeof n);
    counter_value += n;
    memcpy(data, &counter_value, sizeof counter_value);
    return (ssize_t)sizeof counter_value;
}

const struct driver drv = {
    .path         = COUNTER_PATH,
    .version      = DRV_VERSION,
    .fmt_min      = 1,
    .fmt_max      = FMT_MAX,
    .import_state = counter_import,
    .export_state = counter_export,
    .read         = counter_read,
    .write        = counter_write,
    .control      = counter_control,
};
