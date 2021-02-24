// Copyright 2021, Akamai Technologies, Inc.
// SPDX-License-Identifier: Apache-2.0

#define _POSIX_C_SOURCE 200809L

#include "byztime_internal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static pthread_key_t sigbus_key;
static pthread_once_t sigbus_key_once = PTHREAD_ONCE_INIT;
static int sigbus_key_create_result = 0;

static void make_sigbus_key() {
  sigbus_key_create_result = pthread_key_create(&sigbus_key, NULL);
}

int byztime_init_sigbus_key() {
  int ret = pthread_once(&sigbus_key_once, make_sigbus_key);
  if (ret != 0) {
    errno = ret;
    return -1;
  }
  if (sigbus_key_create_result != 0) {
    errno = sigbus_key_create_result;
    return -1;
  }

  return 0;
}

void byztime_handle_sigbus(int signo, siginfo_t *info, void *context) {
  (void)context;
  /* If this isn't a SIGBUS, it's not for us to deal with */
  if (signo != SIGBUS) return;

  /* If this SIGBUS wasn't raised synchronously by an address error,
     it's not for us to deal with and furthermore the the rest of this
     function wouldn't be safe in an asynchronous signal context. */
  if (info->si_code != BUS_ADRERR) return;

  /* If there was an error setting up thread-local storage, there's nothing
     we can do about here so just return. */
  if (byztime_init_sigbus_key() < 0) return;

  /* If the jump buffer hasn't been set up, the SIGBUS happened somewhere
     other than within a libbyztime function accessing the timedata file,
     so it's not for us to deal with. */
  sigjmp_buf *jmpbuf = pthread_getspecific(sigbus_key);
  if (jmpbuf == NULL) return;

  /* If we've gotten this far, we own this. siglongjmp back into the
     function that triggered the page fault. */
  siglongjmp(*jmpbuf, 1);
}

static void sigbus_handler(int signo, siginfo_t *info, void *context) {
  byztime_handle_sigbus(signo, info, context);
  signal(signo, SIG_DFL);
  raise(signo);
}

int byztime_install_sigbus_handler(struct sigaction *oact) {
  struct sigaction sa;
  sa.sa_handler = (void *)sigbus_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO;
  return sigaction(SIGBUS, &sa, oact);
}

byztime_ctx *byztime_open_ro(char const *pathname) {
  byztime_ctx *ctx;
  unsigned char expected_era[BYZTIME_ERA_LEN], stored_era[BYZTIME_ERA_LEN];
  unsigned char stored_magic[BYZTIME_MAGIC_LEN];
  int saved_errno, ret;
  struct stat statbuf;
  sigjmp_buf jmpbuf;

  if (byztime_init_sigbus_key() < 0) return NULL;

  if (byztime_get_clock_era(expected_era) < 0) return NULL;

  ctx = malloc(sizeof(byztime_ctx));
  if (ctx == NULL) return NULL;

  ctx->fd = open(pathname, O_RDONLY, 0);
  if (ctx->fd < 0) goto fail_free_ctx;

  /* Check that the file is the size we expect. This handles common
     cases where the user has pointed us at a zero-byte file by
     mistake, but there's an inherent TOCTOU problem here since in the
     case of faulty behavior by the user that owns the timedata file,
     it could get truncated after our check. To deal properly with
     this, we need to trap and handle SIGBUS. */
  if (fstat(ctx->fd, &statbuf) < 0) goto fail_free_ctx;
  if (statbuf.st_size < (off_t)sizeof(timedata)) {
    errno = EPROTO;
    goto fail_free_ctx;
  }

  ctx->timedata =
      mmap(NULL, sizeof(timedata), PROT_READ, MAP_SHARED, ctx->fd, 0);
  if (ctx->timedata == MAP_FAILED) goto fail_close;

  /* Set up a jump context for the SIGBUS handler to longjmp into. This function
     can return twice. When we first call it it'll return 0. If it returns a
     second time, it's coming from the SIGBUS handler and will return non-zero,
     indicating we've recovered from a truncated timedata file causing a page
     fault. */
  if (sigsetjmp(jmpbuf, 0) != 0) {
    errno = EPROTO;
    goto fail_unmap;
  }

  /* Save a pointer to the jump context that we just set up in thread-local
     storage for the signal handler to look up. */
  ret = pthread_setspecific(sigbus_key, &jmpbuf);
  if (ret != 0) {
    errno = ret;
    goto fail_unmap;
  }

  /* Make sure the compiler doesn't re-order the memory accesses that
     follow such that they occur before we've finished setting up the
     jump context. */
  atomic_signal_fence(memory_order_acq_rel);

  load_magic(stored_magic, &ctx->timedata->magic);
  if (memcmp(stored_magic, expected_magic, sizeof expected_magic)) {
    errno = EPROTO;
    goto fail_unmap;
  }

  load_era(stored_era, &ctx->timedata->era);
  if (memcmp(stored_era, expected_era, sizeof expected_era)) {
    errno = ECONNREFUSED;
    goto fail_unmap;
  }

  ctx->lock_fd = -1;
  ctx->drift_ppb = default_drift_ppb;
  ctx->slew_mode = false;

  /* Make sure the compiler doesn't re-order the above memory accesses
     such that they occur after we've already torn down the jump context. */
  atomic_signal_fence(memory_order_acq_rel);

  /* Tear down the jump context */
  ret = pthread_setspecific(sigbus_key, NULL);
  assert(ret == 0);

  return ctx;

fail_unmap:
  atomic_signal_fence(memory_order_acq_rel);
  saved_errno = errno;
  ret = pthread_setspecific(sigbus_key, NULL);
  assert(ret == 0);
  ret = munmap(ctx->timedata, sizeof(timedata));
  assert(ret == 0);
  errno = saved_errno;
fail_close:
  saved_errno = errno;
  close(ctx->fd);
  errno = saved_errno;
fail_free_ctx:
  free(ctx);
  return NULL;
}

static int get_and_validate_entry(byztime_ctx *ctx, timedata_entry *entry) {
  sigjmp_buf jmpbuf;
  int ret;

  if (sigsetjmp(jmpbuf, 0) != 0) {
    errno = EPROTO;
    return -1;
  }

  ret = pthread_setspecific(sigbus_key, &jmpbuf);
  if (ret != 0) {
    errno = ret;
    return -1;
  }

  atomic_signal_fence(memory_order_acq_rel);

  int i = atomic_load_explicit(&ctx->timedata->i, memory_order_consume);
  if (i < 0 || i >= NUM_ENTRIES) {
    atomic_signal_fence(memory_order_acq_rel);
    ret = pthread_setspecific(sigbus_key, NULL);
    assert(ret == 0);
    errno = EPROTO;
    return -1;
  }

  memcpy(entry, &ctx->timedata->entries[i], sizeof(timedata_entry));

  if (entry->offset.nanoseconds < 0 || entry->offset.nanoseconds >= billion ||
      entry->error.nanoseconds < 0 || entry->error.nanoseconds >= billion ||
      entry->as_of.nanoseconds < 0 || entry->as_of.nanoseconds >= billion) {
    atomic_signal_fence(memory_order_acq_rel);
    ret = pthread_setspecific(sigbus_key, NULL);
    assert(ret == 0);
    errno = EPROTO;
    return -1;
  }

  atomic_signal_fence(memory_order_acq_rel);
  ret = pthread_setspecific(sigbus_key, NULL);
  assert(ret == 0);
  return 0;
}

void byztime_set_drift(byztime_ctx *ctx, int64_t drift_ppb) {
  ctx->drift_ppb = drift_ppb;
}

int64_t byztime_get_drift(byztime_ctx const *ctx) {
  return ctx->drift_ppb;
}

int byztime_slew(byztime_ctx *ctx, int64_t min_rate_ppb, int64_t max_rate_ppb,
                 byztime_stamp const *maxerror) {
  timedata_entry entry;

  if (get_and_validate_entry(ctx, &entry) < 0) { return -1; }

  if (maxerror != NULL && byztime_stamp_cmp(&entry.error, maxerror) > 0) {
    errno = ERANGE;
    return -1;
  }

  ctx->slew_mode = true;
  ctx->slew_have_prev = false;
  ctx->min_rate_ppb = min_rate_ppb;
  ctx->max_rate_ppb = max_rate_ppb;
  return 0;
}

int byztime_step(byztime_ctx *ctx) {
  ctx->slew_mode = false;
  return 0;
}

static int byztime_get_local_time_and_offset(byztime_ctx *ctx,
                                             byztime_stamp *local_time,
                                             byztime_stamp *min,
                                             byztime_stamp *est,
                                             byztime_stamp *max) {
  timedata_entry entry;
  byztime_stamp my_local_time, error, age, scaled_age;
  byztime_stamp my_min, my_max, my_est;

  int64_t drift_ppb_x2;

  if (__builtin_mul_overflow(ctx->drift_ppb, 2, &drift_ppb_x2)) {
    errno = EOVERFLOW;
    return -1;
  }

  if (get_and_validate_entry(ctx, &entry) < 0 ||
      byztime_get_local_time(&my_local_time) < 0 ||
      byztime_stamp_sub(&age, &my_local_time, &entry.as_of) < 0 ||
      byztime_stamp_scale(&scaled_age, &age, drift_ppb_x2) < 0 ||
      byztime_stamp_add(&error, &entry.error, &scaled_age) < 0 ||
      byztime_stamp_sub(&my_min, &entry.offset, &error) < 0 ||
      byztime_stamp_add(&my_max, &entry.offset, &error) < 0) {
    return -1;
  }

  if (ctx->slew_mode) {
    if (ctx->slew_have_prev) {
      byztime_stamp local_time_since_prev, offset_adj_since_prev,
          global_time_since_prev, min_global_time_since_prev,
          max_global_time_since_prev;
      if (byztime_stamp_sub(&local_time_since_prev, &my_local_time,
                            &ctx->prev_local_time) < 0 ||
          byztime_stamp_sub(&offset_adj_since_prev, &entry.offset,
                            &ctx->prev_offset) < 0 ||
          byztime_stamp_add(&global_time_since_prev, &local_time_since_prev,
                            &offset_adj_since_prev) < 0 ||
          byztime_stamp_scale(&min_global_time_since_prev,
                              &global_time_since_prev, ctx->min_rate_ppb) < 0 ||
          (ctx->max_rate_ppb < INT64_MAX &&
           byztime_stamp_scale(&max_global_time_since_prev,
                               &global_time_since_prev,
                               ctx->max_rate_ppb) < 0)) {
        return -1;
      }

      if (byztime_stamp_cmp(&global_time_since_prev,
                            &min_global_time_since_prev) < 0) {
        byztime_stamp shortfall_global_time_since_prev;
        if (byztime_stamp_sub(&shortfall_global_time_since_prev,
                              &min_global_time_since_prev,
                              &global_time_since_prev) < 0 ||
            byztime_stamp_add(&my_est, &entry.offset,
                              &shortfall_global_time_since_prev) < 0) {
          return -1;
        }
      } else if (ctx->max_rate_ppb < INT64_MAX &&
                 byztime_stamp_cmp(&global_time_since_prev,
                                   &max_global_time_since_prev) > 0) {
        byztime_stamp excess_global_time_since_prev;
        if (byztime_stamp_sub(&excess_global_time_since_prev,
                              &global_time_since_prev,
                              &max_global_time_since_prev) < 0 ||
            byztime_stamp_sub(&my_est, &entry.offset,
                              &excess_global_time_since_prev) < 0) {
          return -1;
        }
      } else {
        my_est = entry.offset;
      }
    } else {
      my_est = entry.offset;
    }

    ctx->prev_local_time = my_local_time;
    ctx->prev_offset = my_est;
    ctx->slew_have_prev = true;
  } else {
    my_est = entry.offset;
  }

  if (local_time != NULL) *local_time = my_local_time;
  if (min != NULL) *min = my_min;
  if (est != NULL) *est = my_est;
  if (max != NULL) *max = my_max;
  return 0;
}

int byztime_get_offset(byztime_ctx *ctx, byztime_stamp *min, byztime_stamp *est,
                       byztime_stamp *max) {
  return byztime_get_local_time_and_offset(ctx, NULL, min, est, max);
}

int byztime_get_global_time(byztime_ctx *ctx, byztime_stamp *min,
                            byztime_stamp *est, byztime_stamp *max) {
  byztime_stamp local_time, my_min, my_est, my_max;

  if (byztime_get_local_time_and_offset(ctx, &local_time, &my_min, &my_est,
                                        &my_max) < 0 ||
      byztime_stamp_add(&my_min, &my_min, &local_time) < 0 ||
      byztime_stamp_add(&my_est, &my_est, &local_time) < 0 ||
      byztime_stamp_add(&my_max, &my_max, &local_time) < 0) {
    return -1;
  }

  if (min != NULL) *min = my_min;
  if (est != NULL) *est = my_est;
  if (max != NULL) *max = my_max;
  return 0;
}
