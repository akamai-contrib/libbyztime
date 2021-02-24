// Copyright 2021, Akamai Technologies, Inc.
// SPDX-License-Identifier: Apache-2.0

#define _POSIX_C_SOURCE 200809L
#include "byztime_internal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int byztime_close(byztime_ctx *ctx) {
  int ret, saved_errno;
  if (ctx == NULL) return 0;

  ret = munmap(ctx->timedata, sizeof(timedata));
  assert(ret == 0);
  ret = fsync(ctx->fd);
  saved_errno = errno;
  if (close(ctx->fd) < 0) { assert(errno == EINTR); }
  if (ctx->lock_fd >= 0) {
    if (close(ctx->lock_fd) < 0) { assert(errno == EINTR); }
  }
  free(ctx);
  errno = saved_errno;
  return ret;
}

#define BOOT_ID_LEN 36

int byztime_get_clock_era(unsigned char era[16]) {
  char boot_id[BOOT_ID_LEN + 1];
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY, 0);
  int ret, saved_errno;
  if (fd < 0) return -1;
  do {
    ret = read(fd, boot_id, BOOT_ID_LEN);
  } while (ret < 0 && errno == EINTR);
  if (ret < 0) {
    saved_errno = errno;
    if (close(fd) < 0) { assert(errno == EINTR); }
    errno = saved_errno;
    return -1;
  }

  assert(ret == BOOT_ID_LEN);
  boot_id[BOOT_ID_LEN] = '\0';

  ret = sscanf(boot_id,
               "%2hhx%2hhx%2hhx%2hhx-%2hhx%2hhx-%2hhx%2hhx-%2hhx%2hhx-%2hhx%"
               "2hhx%2hhx%2hhx%2hhx%2hhx",
               era, era + 1, era + 2, era + 3, era + 4, era + 5, era + 6,
               era + 7, era + 8, era + 9, era + 10, era + 11, era + 12,
               era + 13, era + 14, era + 15);
  assert(ret == 16);
  if (close(fd) < 0) { assert(errno == EINTR); }
  return 0;
}

int byztime_get_local_time(byztime_stamp *local_time) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) < 0) return -1;

  local_time->seconds = ts.tv_sec;
  local_time->nanoseconds = ts.tv_nsec;
  return byztime_stamp_normalize(local_time);
}

int byztime_get_real_time(byztime_stamp *real_time) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return -1;

  real_time->seconds = ts.tv_sec;
  real_time->nanoseconds = ts.tv_nsec;
  return byztime_stamp_normalize(real_time);
}
