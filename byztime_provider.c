// Copyright 2021, Akamai Technologies, Inc.
// SPDX-License-Identifier: Apache-2.0

#define _POSIX_C_SOURCE 200112L
#include "byztime_internal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The lock-free algorithm that we use to access timedata means that
   no reader-locks ever have to be acquired. But simulataneous writers
   still have to be avoided. We have two lines of defense against this.

   The first line of defense is file-locking using flock(). This
   protects us from the common case of the user accidentally starting
   one daemon while another is still running. We acquire the lock not
   on the timedata file itself, but a separate file in the same
   directory with the suffix ".lock". This file has permissions 0600
   rather than 0644, preventing untrusted users from DoSing writers by
   sitting on a reader lock forever.

   The second line of defense is a mutex inside the timedata
   file. This protects us from undefined behavior when two threads try
   to update it simultaneously. This should never really happen in the
   first place, and if the whole world were C then "don't do that"
   would be a sufficient answer, but we want safe languages to be able
   to wrap this library and provide a memory-safe interface to it.
   By putting the mutex inside the timedata file rather than someplace
   else, we can ensure safety even when a process forks after opening
   the timedata file, giving the parent and child no other memory in
   common for coordination. The mutex is taken before making any change
   to the timedata file and then immediately released afterward.

   The mutex is re-initialized every time we open the timedata file
   read-write. At this point, due to the file-lock, we know we're the
   only writer, so it's safe to do this. The re-initialization
   prevents us from getting permanently wedged if a process dies while
   holding the mutex.
*/

static int acquire_lock(char const *pathname) {
  char lock_pathname[PATH_MAX];
  int lock_fd;
  if (realpath(pathname, lock_pathname) == NULL) { return -1; }

  if (strlen(lock_pathname) + strlen(".lock") + 1 > PATH_MAX) {
    errno = ENAMETOOLONG;
    return -1;
  }

  strcat(lock_pathname, ".lock");

  lock_fd = open(lock_pathname, O_RDWR | O_CREAT, 0600);
  if (lock_fd < 0) { return -1; }

  if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) { return -1; }

  return lock_fd;
}

/* Invariants to be maintained while the timedata file is being
   updated or initalized:

   1. If the magic is correct, the rest of the file is well-formed.
   2. If the era is current, then the entry indexed by i is valid.

   So on first initialization, set the magic last. On
   re-initialization when the magic is already valid, update the era
   last. Calls to atomic_thread_fence() within
   load_era/store_era/load_magic/store_magic ensure that changes
   really are seen in the order we make them.
*/

byztime_ctx *byztime_open_rw(char const *pathname) {
  byztime_ctx *ctx;
  unsigned char expected_era[BYZTIME_ERA_LEN], stored_era[BYZTIME_ERA_LEN];
  unsigned char stored_magic[BYZTIME_MAGIC_LEN];
  int saved_errno, ret;

  if (byztime_init_sigbus_key() < 0) return NULL;

  if (byztime_get_clock_era(expected_era) < 0) return NULL;

  ctx = malloc(sizeof(byztime_ctx));
  if (ctx == NULL) return NULL;

  ctx->fd = open(pathname, O_RDWR | O_CREAT, 0644);
  if (ctx->fd < 0) goto fail_free_ctx;

  ctx->lock_fd = acquire_lock(pathname);
  if (ctx->lock_fd < 0) goto fail_close;

  if ((errno = posix_fallocate(ctx->fd, 0, sizeof(timedata))) < 0) {
    goto fail_release_lock;
  }

  ctx->timedata = mmap(NULL, sizeof(timedata), PROT_READ | PROT_WRITE,
                       MAP_SHARED, ctx->fd, 0);
  if (ctx->timedata == MAP_FAILED) goto fail_release_lock;

  load_magic(stored_magic, &ctx->timedata->magic);
  if (memcmp(stored_magic, expected_magic, sizeof expected_magic) ||
      atomic_load(&ctx->timedata->i) < 0 ||
      atomic_load(&ctx->timedata->i) >= NUM_ENTRIES) {
    /* First-time initialization of timedata */
    timedata_entry entry;
    byztime_stamp local_time, real_time;

    ctx->timedata->real_offset.seconds = 0;
    ctx->timedata->real_offset.nanoseconds = 0;

    if (byztime_get_local_time(&local_time) < 0 ||
        byztime_get_real_time(&real_time) < 0 ||
        byztime_stamp_sub(&entry.offset, &real_time, &local_time) < 0) {
      goto fail_unmap;
    }
    entry.as_of = local_time;
    entry.error = (byztime_stamp){INT64_MAX >> 1, 0};

    ctx->timedata->entries[0] = entry;
    atomic_init(&ctx->timedata->i, 0);
    store_era(&ctx->timedata->era, expected_era);
    store_magic(&ctx->timedata->magic, expected_magic);
  } else {
    load_era(stored_era, &ctx->timedata->era);
    if (memcmp(stored_era, expected_era, sizeof expected_era)) {
      /* Re-initializiation of timedata following a reboot */
      timedata_entry entry;
      byztime_stamp local_time, real_time, global_time;

      if (byztime_get_local_time(&local_time) < 0 ||
          byztime_get_real_time(&real_time) < 0 ||
          byztime_stamp_add(&global_time, &real_time,
                            &ctx->timedata->real_offset) < 0 ||
          byztime_stamp_sub(&entry.offset, &global_time, &local_time) < 0) {
        goto fail_unmap;
      }

      entry.as_of = local_time;
      entry.error = (byztime_stamp){INT64_MAX >> 1, 0};

      ctx->timedata->entries[0] = entry;
      atomic_init(&ctx->timedata->i, 0);
      store_era(&ctx->timedata->era, expected_era);
    }
  }

  ctx->drift_ppb = default_drift_ppb;
  ctx->slew_mode = false;

  pthread_mutexattr_t mutex_attr;

  ret = pthread_mutexattr_init(&mutex_attr);
  if (ret != 0) {
    errno = ret;
    goto fail_unmap;
  }

  ret = pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
  if (ret != 0) {
    errno = ret;
    goto fail_unmap;
  }

  ret = pthread_mutex_init(&ctx->timedata->mutex, &mutex_attr);
  if (ret != 0) {
    errno = ret;
    goto fail_unmap;
  }

  return ctx;

fail_unmap:
  saved_errno = errno;
  ret = munmap(ctx->timedata, sizeof(timedata));
  assert(ret == 0);
  errno = saved_errno;
fail_release_lock:
  saved_errno = errno;
  close(ctx->lock_fd);
  errno = saved_errno;
fail_close:
  saved_errno = errno;
  close(ctx->fd);
  errno = saved_errno;
fail_free_ctx:
  free(ctx);
  return NULL;
}

int byztime_set_offset(byztime_ctx *ctx, byztime_stamp const *offset,
                       byztime_stamp const *maxerror,
                       byztime_stamp const *as_of) {
  timedata_entry entry;
  int ret;

  memset(&entry, 0, sizeof entry);

  entry.offset = *offset;
  entry.error = *maxerror;

  if (as_of == NULL) {
    byztime_stamp local_time;
    if (byztime_get_local_time(&local_time) < 0) return -1;
    entry.as_of = local_time;
  } else {
    entry.as_of = *as_of;
  }

  ret = pthread_mutex_lock(&ctx->timedata->mutex);
  if (ret != 0) {
    errno = ret;
    return -1;
  }
  int i = atomic_load_explicit(&ctx->timedata->i, memory_order_acquire) + 1;
  if (i == NUM_ENTRIES) i = 0;
  ctx->timedata->entries[i] = entry;
  atomic_store_explicit(&ctx->timedata->i, i, memory_order_release);
  ret = pthread_mutex_unlock(&ctx->timedata->mutex);
  if (ret != 0) {
    errno = ret;
    return -1;
  }

  return 0;
}

void byztime_get_offset_quick(byztime_ctx const *ctx, byztime_stamp *offset) {
  *offset = ctx->timedata->entries[ctx->timedata->i].offset;
}

void byztime_get_offset_raw(byztime_ctx const *ctx, byztime_stamp *offset,
                            byztime_stamp *error, byztime_stamp *as_of) {
  if (offset != NULL) *offset = ctx->timedata->entries[ctx->timedata->i].offset;
  if (error != NULL) *error = ctx->timedata->entries[ctx->timedata->i].error;
  if (as_of != NULL) *as_of = ctx->timedata->entries[ctx->timedata->i].as_of;
}

int byztime_update_real_offset(byztime_ctx *ctx) {
  byztime_stamp real_time, global_time;
  int ret;

  if (byztime_get_global_time(ctx, NULL, &global_time, NULL) < 0 ||
      byztime_get_real_time(&real_time)) {
    return -1;
  }

  ret = pthread_mutex_lock(&ctx->timedata->mutex);
  if (ret != 0) {
    errno = ret;
    return -1;
  }

  if (byztime_stamp_sub(&ctx->timedata->real_offset, &global_time, &real_time) <
      0) {
    pthread_mutex_unlock(&ctx->timedata->mutex);
    return -1;
  }

  ret = pthread_mutex_unlock(&ctx->timedata->mutex);
  if (ret != 0) {
    errno = ret;
    return -1;
  }

  return 0;
}
