// Copyright 2021, Akamai Technologies, Inc.
// SPDX-License-Identifier: Apache-2.0

#ifndef BYZTIME_INTERNAL_H_
#define BYZTIME_INTERNAL_H_

#include "byztime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define BYZTIME_MAGIC_LEN 12

typedef struct magic_s {
  atomic_int magic[BYZTIME_MAGIC_LEN >> 2];
} magic;

typedef struct era_s {
  atomic_int era[BYZTIME_ERA_LEN >> 2];
} era;

typedef struct timedata_entry_s {
  union {
    struct {
      byztime_stamp offset;
      byztime_stamp error;
      byztime_stamp as_of;
    };
    char padding[64];
  };
} timedata_entry;

/* Chosen so that the timedata file will be 4096 bytes long, which is
   one memory page.*/
#define NUM_ENTRIES 62

typedef struct timedata_s {
  union {
    struct {
      magic magic;
      atomic_int i;
      era era;
      byztime_stamp real_offset;
      pthread_mutex_t mutex;
    };
    char padding[128];
  };
  timedata_entry entries[NUM_ENTRIES];
} timedata;

_Static_assert(sizeof(timedata) == 4096,
               "timedata is expected to have size 4096");

struct byztime_ctx_s {
  int fd, lock_fd;
  timedata __attribute__((aligned(16))) * timedata;
  int64_t drift_ppb;

  int64_t min_rate_ppb;
  int64_t max_rate_ppb;
  byztime_stamp prev_local_time;
  byztime_stamp prev_offset;
  bool slew_mode;
  bool slew_have_prev;
};

static const int64_t default_drift_ppb = 250000;
static const int billion = 1000000000;
static const unsigned char expected_magic[BYZTIME_MAGIC_LEN] = {
    'B', 'Y', 'Z', 'T', 'I', 'M', 'E', 0x00, 0xff, 0xff, 0xff, 0xff};
static const byztime_stamp zerostamp = {0, 0};

static inline void load_era(unsigned char out[BYZTIME_ERA_LEN], era const *in) {
  atomic_thread_fence(memory_order_acquire);
  for (int i = 0; i < (BYZTIME_ERA_LEN >> 2); i++) {
    unsigned int word =
        (unsigned int)atomic_load_explicit(&in->era[i], memory_order_relaxed);
    out[4 * i] = word & 0xff;
    out[4 * i + 1] = (word >> 8) & 0xff;
    out[4 * i + 2] = (word >> 16) & 0xff;
    out[4 * i + 3] = (word >> 24) & 0xff;
  }
}

static inline void store_era(era *out,
                             const unsigned char in[BYZTIME_ERA_LEN]) {
  for (int i = 0; i < (BYZTIME_ERA_LEN >> 2); i++) {
    unsigned int word = (unsigned int)in[4 * i] |
                        (unsigned int)(in[4 * i + 1] << 8) |
                        (unsigned int)(in[4 * i + 2] << 16) |
                        (unsigned int)(in[4 * i + 3] << 24);
    atomic_store_explicit(&out->era[i], (int)word, memory_order_relaxed);
  }
  atomic_thread_fence(memory_order_release);
}

static inline void load_magic(unsigned char out[BYZTIME_MAGIC_LEN],
                              magic const *in) {
  atomic_thread_fence(memory_order_acquire);
  for (int i = 0; i < (BYZTIME_MAGIC_LEN >> 2); i++) {
    unsigned int word =
        (unsigned int)atomic_load_explicit(&in->magic[i], memory_order_relaxed);
    out[4 * i] = word & 0xff;
    out[4 * i + 1] = (word >> 8) & 0xff;
    out[4 * i + 2] = (word >> 16) & 0xff;
    out[4 * i + 3] = (word >> 24) & 0xff;
  }
}

static inline void store_magic(magic *out,
                               const unsigned char in[BYZTIME_MAGIC_LEN]) {
  for (int i = 0; i < (BYZTIME_MAGIC_LEN >> 2); i++) {
    unsigned int word = (unsigned int)in[4 * i] |
                        (unsigned int)(in[4 * i + 1] << 8) |
                        (unsigned int)(in[4 * i + 2] << 16) |
                        (unsigned int)(in[4 * i + 3] << 24);
    atomic_store_explicit(&out->magic[i], (int)word, memory_order_relaxed);
  }
  atomic_thread_fence(memory_order_release);
}

int byztime_init_sigbus_key();

#endif
