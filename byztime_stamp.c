// Copyright 2021, Akamai Technologies, Inc.
// SPDX-License-Identifier: Apache-2.0

#define _POSIX_C_SOURCE 200809L
#include "byztime_internal.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int byztime_stamp_normalize(byztime_stamp *stamp) {
  /* Common-case optimization */
  if (stamp->nanoseconds >= 0 && stamp->nanoseconds < billion) { return 0; }

  int64_t nsec_div = stamp->nanoseconds / billion;
  int64_t nsec_mod = stamp->nanoseconds % billion;
  bool overflowed =
      __builtin_add_overflow(stamp->seconds, nsec_div, &stamp->seconds);
  stamp->nanoseconds = nsec_mod;

  if (stamp->nanoseconds < 0) {
    overflowed |= __builtin_sub_overflow(stamp->seconds, 1, &stamp->seconds);
    stamp->nanoseconds = stamp->nanoseconds + billion;
  }

  if (overflowed) {
    errno = EOVERFLOW;
    return -1;
  } else {
    return 0;
  }
}
int byztime_stamp_add(byztime_stamp *sum, byztime_stamp const *stamp1,
                      byztime_stamp const *stamp2) {
  bool overflow = false;
  byztime_stamp s1 = *stamp1, s2 = *stamp2;

  if (byztime_stamp_normalize(&s1) < 0) {
    assert(errno == EOVERFLOW);
    overflow = true;
  }

  if (byztime_stamp_normalize(&s2) < 0) {
    assert(errno == EOVERFLOW);
    overflow = true;
  }

  overflow |= __builtin_add_overflow(s1.seconds, s2.seconds, &sum->seconds);
  sum->nanoseconds = s1.nanoseconds + s2.nanoseconds;

  if (byztime_stamp_normalize(sum) < 0) {
    assert(errno == EOVERFLOW);
    overflow = true;
  }

  if (overflow) {
    errno = EOVERFLOW;
    return -1;
  }

  return 0;
}

int byztime_stamp_sub(byztime_stamp *diff, byztime_stamp const *stamp1,
                      byztime_stamp const *stamp2) {
  bool overflow = false;
  byztime_stamp s1 = *stamp1, s2 = *stamp2;

  if (byztime_stamp_normalize(&s1) < 0) {
    assert(errno == EOVERFLOW);
    overflow = true;
  }

  if (byztime_stamp_normalize(&s2) < 0) {
    assert(errno == EOVERFLOW);
    overflow = true;
  }

  overflow |= __builtin_sub_overflow(s1.seconds, s2.seconds, &diff->seconds);
  diff->nanoseconds = s1.nanoseconds - s2.nanoseconds;

  if (byztime_stamp_normalize(diff) < 0) {
    assert(errno == EOVERFLOW);
    overflow = true;
  }

  if (overflow) {
    errno = EOVERFLOW;
    return -1;
  }

  return 0;
}

/** Faster version of byztime_stamp_scale for when 0 <= ppb <= 1_000_000_000. */
static int byztime_stamp_downscale(byztime_stamp *prod,
                                   byztime_stamp const *stamp, int64_t ppb) {
  byztime_stamp s = *stamp;
  bool overflowed = false;

  if (byztime_stamp_normalize(&s) < 0) {
    assert(errno == EOVERFLOW);
    overflowed = true;
  }

  int64_t gigaseconds_in = s.seconds / billion;
  int64_t seconds_in = s.seconds % billion;
  int64_t nanoseconds_in = s.nanoseconds;

  int64_t nanoparts = ppb;
  assert(nanoparts >= 0 && nanoparts <= billion);

  int64_t seconds_out = gigaseconds_in * nanoparts;
  ;
  int64_t nanoseconds_out = seconds_in * nanoparts;
  int64_t attoseconds_out = nanoseconds_in * nanoparts;

  prod->seconds = seconds_out;
  prod->nanoseconds = nanoseconds_out + attoseconds_out / billion;

  int64_t residue = attoseconds_out % billion;
  if (residue > (billion >> 1) ||
      (residue == (billion >> 1) && (prod->nanoseconds & 1))) {
    prod->nanoseconds++;
  } else if (residue < -(billion >> 1) ||
             (residue == -(billion >> 1) && (prod->nanoseconds & 1))) {
    prod->nanoseconds--;
  }

  if (byztime_stamp_normalize(prod) < 0) { assert(false); }

  if (overflowed) {
    errno = EOVERFLOW;
    return -1;
  } else {
    return 0;
  }
}

int byztime_stamp_scale(byztime_stamp *prod, byztime_stamp const *stamp,
                        int64_t ppb) {
  if (ppb >= 0 && ppb <= billion) {
    return byztime_stamp_downscale(prod, stamp, ppb);
  }

  byztime_stamp s = *stamp;
  bool overflowed = false;

  if (byztime_stamp_normalize(&s) < 0) {
    assert(errno == EOVERFLOW);
    overflowed = true;
  }

  /* We'll do schoolbook multiplication where these three places... */
  int64_t gigaseconds_in = s.seconds / billion;
  int64_t seconds_in = s.seconds % billion;
  int64_t nanoseconds_in = s.nanoseconds;

  /* ...are each multiplied by each these two places... */
  int64_t parts = ppb / billion;
  int64_t nanoparts = ppb % billion;

  /* ...producing six outputs: one on the scale of gigaseconds, two on
     the scale of seconds, two on the scale of nanoseconds, and one on
     the scale of attoseconds */
  int64_t gigaseconds_out;
  int64_t seconds_out_1, seconds_out_2;
  int64_t nanoseconds_out_1, nanoseconds_out_2;
  int64_t attoseconds_out;

  overflowed |= __builtin_mul_overflow(gigaseconds_in, parts, &gigaseconds_out);

  /* The following operations can't overflow because their factors are
     either a quotient (Q) and a remainder (R) from division by one
     billion, or two remainders from the same */
  seconds_out_1 = seconds_in /*R*/ * parts /*Q*/;
  seconds_out_2 = gigaseconds_in /*Q*/ * nanoparts /*R*/;
  nanoseconds_out_1 = seconds_in /*R*/ * nanoparts /*R*/;
  nanoseconds_out_2 = nanoseconds_in /*R*/ * parts /*Q*/;
  attoseconds_out = nanoseconds_in /*R*/ * nanoparts /*R*/;

  /* We multiply the gigaseconds place by one billion to convert it to seconds,
     and then add it to the other seconds-scale outputs to get the product
     seconds field. */
  overflowed |=
      __builtin_mul_overflow(gigaseconds_out, billion, &prod->seconds);
  overflowed |=
      __builtin_add_overflow(prod->seconds, seconds_out_1, &prod->seconds);
  overflowed |=
      __builtin_add_overflow(prod->seconds, seconds_out_2, &prod->seconds);

  /* We divide the attoseconds place by one billion to convert it to
     nanoseconds, and add it to the other nanosecond-scale outputs to
     get the product nanoseconds field.

     attoseconds_out / billion is bounded by one billion, and
     nanoseconds_out_1 is bounded by one quintiillion, but
     nanoseconds_out_2 might be as big as INT64_MAX. So to prevent an
     unnecessary overflow, we need to first set the nanoseconds to
     nanoseconds_out_2 and then renormalize before adding in the other
     terms. But afterwards, the addition can be unchecked. */
  prod->nanoseconds = nanoseconds_out_2;
  if (byztime_stamp_normalize(prod) < 0) {
    assert(errno == EOVERFLOW);
    overflowed = true;
  }
  prod->nanoseconds += attoseconds_out / billion + nanoseconds_out_1;

  /* Now we possibly make a +/- 1 adjustment due to rounding. This can
     again be unchecked since we haven't added anything large to our
     nanoseconds since having last normalized. */
  int64_t residue = attoseconds_out % billion;
  if (residue > (billion >> 1) ||
      (residue == (billion >> 1) && (prod->nanoseconds & 1))) {
    prod->nanoseconds++;
  } else if (residue < -(billion >> 1) ||
             (residue == -(billion >> 1) && (prod->nanoseconds & 1))) {
    prod->nanoseconds--;
  }

  /* Now we're finished after normalizing for the final time. */
  if (byztime_stamp_normalize(prod) < 0) {
    assert(errno == EOVERFLOW);
    overflowed = true;
  }

  if (overflowed) {
    errno = EOVERFLOW;
    return -1;
  } else {
    return 0;
  }
}

void byztime_stamp_halve(byztime_stamp *prod, byztime_stamp const *stamp) {
  int64_t seconds = stamp->seconds;
  int64_t nanoseconds = stamp->nanoseconds;

  prod->seconds = seconds >> 1;
  prod->nanoseconds = nanoseconds >> 1;
  if (seconds & 1) prod->nanoseconds += 500000000;
  if ((nanoseconds & 3) == 3) {
    if (nanoseconds > 0) {
      prod->nanoseconds++;
    } else {
      prod->nanoseconds--;
    }
  }
}

int byztime_stamp_cmp(byztime_stamp const *stamp1,
                      byztime_stamp const *stamp2) {
  byztime_stamp s1 = *stamp1, s2 = *stamp2;

  (void)byztime_stamp_normalize(&s1);
  (void)byztime_stamp_normalize(&s2);

  if (s1.seconds < s2.seconds) return -1;
  if (s1.seconds > s2.seconds) return 1;
  if (s1.nanoseconds < s2.nanoseconds) return -1;
  if (s1.nanoseconds > s2.nanoseconds) return 1;
  return 0;
}

size_t byztime_stamp_fmt(char *str, size_t size, byztime_stamp const *stamp) {
  byztime_stamp s = *stamp;
  int ret;
  (void)byztime_stamp_normalize(&s);

  if (stamp->seconds >= 0 || stamp->nanoseconds == 0) {
    ret =
        snprintf(str, size, "%" PRId64 ".%.9" PRId64, s.seconds, s.nanoseconds);
    assert(ret >= 0);
    return (size_t)ret;
  } else {
    ret = snprintf(str, size, "%" PRId64 ".%.9" PRId64, s.seconds + 1,
                   (uint64_t)(billion - s.nanoseconds));
    assert(ret >= 0);
    return (size_t)ret;
  }
}
