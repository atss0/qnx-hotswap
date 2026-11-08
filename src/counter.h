/*
 * counter.h - the record returned by the counter driver (/dev/counter).
 * Shared by the driver (counter.c) and its test client (client.c).
 */
#ifndef COUNTER_H
#define COUNTER_H

#include <stdint.h>

#define COUNTER_PATH "/dev/counter"

/* One read() of /dev/counter returns exactly this record. */
struct counter_rec {
    uint64_t value;         /* counter value; +1 on every read */
    uint32_t core_version;  /* version of the driver that served the read */
    uint32_t reserved;
};

#endif /* COUNTER_H */
