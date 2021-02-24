/* Copyright 2021, Akamai Technologies, Inc.
    SPDX-License-Identifier: Apache-2.0
*/

#ifndef BYZTIME_H_
#define BYZTIME_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <stdatomic.h>
#if ATOMIC_INT_LOCK_FREE < 2
#error                                                                         \
    "This platform does not support lock-free atomic stores on ints, which libbyztime relies upon"
#endif

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
#include <signal.h>
#endif

/** \file byztime.h */
/** \defgroup stamp Timestamp manipulation
    @{
*/

/** The type of timestamps. On modern systems this is isomorphic to
    `struct timespec`, but fields are always 64 bits even where
    `time_t` is 32 bits.*/
typedef struct byztime_stamp_s {
  int64_t seconds;
  int64_t nanoseconds;
} byztime_stamp;

/** Normalizes a timestamp so that its `nanoseconds` field is in the range
   [0,1000000000).

   \param[in,out] stamp The timestamp to be normalized.

   \returns 0 on success.
   \returns -1 on failure and sets `errno`.

   \exception EOVERFLOW The operation caused an integer overflow/underflow,
   and was completed with 2-complement wraparound semantics.
*/
int byztime_stamp_normalize(byztime_stamp *stamp);

/** Adds two timestamps.

    \param[out] sum The sum of `stamp1` and `stamp2`.
    \param[in] stamp1 The first summand.
    \param[in] stamp2 The second summand.

   \returns 0 on success.
   \returns -1 on failure and sets `errno`.

   \exception EOVERFLOW The operation caused an integer overflow/underflow,
   and was completed with 2-complement wraparound semantics.
*/
int byztime_stamp_add(byztime_stamp *sum, byztime_stamp const *stamp1,
                      byztime_stamp const *stamp2);

/** Substracts two timestamps.

    \param[out] diff The difference of `stamp1` and `stamp2`.
    \param[in] stamp1 The minuend.
    \param[in] stamp2 The subtrahend.

   \returns 0 on success.
   \returns -1 on failure and sets `errno`.

   \exception EOVERFLOW The operation caused an integer overflow/underflow,
   and was completed with 2-complement wraparound semantics.
*/
int byztime_stamp_sub(byztime_stamp *diff, byztime_stamp const *stamp1,
                      byztime_stamp const *stamp2);

/** Compares the normalized form two timestamps.

    \param[in] stamp1 The first timestamp to be compared.
    \param[out] stamp2 The second timestamp to be compared.

   This function has no means of signaling whether normalizing the inputs
   resulted in an overflow. In case of concern, pass inputs that are already
   normalized.

   \returns -1 if stamp1 < stamp2
   \returns 0 if stamp1 == stamp2
   \returns 1 if stamp1 > stamp2
*/
int byztime_stamp_cmp(byztime_stamp const *stamp1, byztime_stamp const *stamp2);

/** Scales (i.e. multiplies) a timestamp.

    \param[out] prod The scaled timestamp.
    \param[in] stamp The timestamp to be scaled.
    \param[ppb] ppb The amount by which to scale `stamp`, in parts per billion.

   \returns 0 on success.
   \returns -1 on failure and sets `errno`.

   \exception EOVERFLOW The operation caused an integer overflow/underflow,
   and was completed with 2-complement wraparound semantics.
 */
int byztime_stamp_scale(byztime_stamp *prod, byztime_stamp const *stamp,
                        int64_t ppb);

/** Halves a timestamp.

    \param[out] prod The scaled timestamp.
    \param[in] stamp The timestamp to be scaled.

    This function is much faster than calling byztime_stamp_scale() with
    ppb=500_000_000.

    If `stamp` is non-normalized then `prod` may be non-normalized as well.

    \returns void. This function always succeeds.
*/
void byztime_stamp_halve(byztime_stamp *prod, byztime_stamp const *stamp);

/** The maximum number of bytes written by byztime_stamp_fmt(). */
#define BYZTIME_STAMP_MAX_FMT_LEN 32

/** Formats a timestamp.

 \param[out] str Pointer to the output buffer.
 \param[in] size Size of the output buffer.
 \param[in] stamp The timestamp to be formatted.

 A buffer of size at least `BYZTIME_STAMP_MAX_FMT_LEN` will always be able
 to contain the formatted timestamp without truncation.

 \return The number of bytes that were needed to represent `stamp`, excluding
 the terminating null. If the return value is greater than `size-1`, the
 output was truncated.
*/

size_t byztime_stamp_fmt(char *str, size_t size, byztime_stamp const *stamp);

/** @} */
/** \defgroup common Common API for consumers and providers
    @{
*/

/** The type of context objects used for provider-consumer communication. */
typedef struct byztime_ctx_s byztime_ctx;

#define BYZTIME_ERA_LEN 16

/** Gets the current clock era.

    This is a random 16-byte string that changes after a reboot but otherwise
    remains constant.

    \param era 16-byte buffer into which to return the era.

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.
*/
int byztime_get_clock_era(unsigned char era[BYZTIME_ERA_LEN]);

/** Gets the current local time.

    This is a clock value that advances monotonically from some
    arbitrary epoch.  It is comparable only with other local clock
    values obtained from the same machine with no intervening reboot.

    \param[out] local_time The current local time.

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.
*/
int byztime_get_local_time(byztime_stamp *local_time);

/** Get the current real time relative to the POSIX epoch.

    \param[out] real_time The current real time.

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.
*/
int byztime_get_real_time(byztime_stamp *real_time);

/** Closes a context object.

    \param[in] ctx Pointer to the context object to be closed.

    Using a context object after closing it will result in undefined behavior.

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.
*/
int byztime_close(byztime_ctx *ctx);

/** @} */
/** \defgroup consumer Consumer API
    @{
*/

/** Opens a timedata file for read-only access.

    \param[in] pathname The path to the timedata file.

    \return A pointer to a newly-allocated context object, or `NULL` on failure
    and sets `errno`.

    In addition to any `errno` values set by the operating system, this function
    may set `errno` as follows.

    \exception EPROTO `pathname` is not a correctly-formatted timedata file.
    \exception ECONNREFUSED The timedata file's era does not match the current
    boot. This usually indicates that byztimed is not running.
*/
byztime_ctx *byztime_open_ro(char const *pathname);

/** Gets bounds and an estimate of time offset `(global time - local time)`.

    \param[in] ctx Pointer to context object.
    \param[out] min Minimum possible offset.
    \param[out] est Estimated offset.
    \param[out] max Maximum possible offset.

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.

    \exception EPROTO The timedata file is improperly formatted.
    \exception EOVERFLOW An integer underflow/overflow occurred during
    offset or error computation. The resulting output values are
    undefined.

    In addition to the above `errno` values, any error set by `clock_gettime()`
    may be returned. `clock_gettime()` will never be called if `error` is `NULL`
    or the drift rate is set to 0.
*/
int byztime_get_offset(byztime_ctx *ctx, byztime_stamp *min, byztime_stamp *est,
                       byztime_stamp *max);

/** Gets bounds and an estimate of the global time.

    \param[in] ctx Pointer to context object.
    \param[out] min Minimum possible global time.
    \param[out] est Estimated global time.
    \param[out] max Maximum possible global time.

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.

    \exception EPROTO The timedata file is improperly formatted.
    \exception EOVERFLOW An integer underflow/overflow occurred during
    time or error computation. The resulting output values are
    undefined.

    In addition to the above `errno` values, any error set by `clock_gettime()`
    may be returned.

    It is important to be aware that `min` and `max` are bounds on the
    *actual* global time, not on other nodes' estimation thereof. It
    is guranteed that other correct nodes' ranges will overlap ours,
    that is, their `min` will be less than our `max`, and their `max`
    will be greater than our `min`. However, it is *not* guaranteed
    that other correct nodes' `est` will be between our `min` and our
    `max`.
*/
int byztime_get_global_time(byztime_ctx *ctx, byztime_stamp *min,
                            byztime_stamp *est, byztime_stamp *max);

/** Sets the drift rate used in error calculations.

    \param[in] ctx Pointer to context object.
    \param[in] drift_ppb Drift rate in parts per billion.
*/
void byztime_set_drift(byztime_ctx *ctx, int64_t drift_ppb);

/** Returns the drift rate used in error calculations.

    \param[in] ctx Pointer to context object.
*/
int64_t byztime_get_drift(byztime_ctx const *ctx);

/** Begin slewing time estimates.

    This function changes how `est` is calcuated in future calls to
    `byztime_get_offset()` and `byztime_get_global_time()`. When a context
    is first opened, time estimation is in "step" mode where the estimate
    is always the midpoint of the min and max. Such an estimate changes
    discontinuously every time a new data point is obtained, and can move
    backward.

    Calling this function causes future estimates to be clamped such
    that they will be more consistent with each other. Specifically,
    if `byztime_get_global_time()` returns an estimate of `g_1` at
    local time `l_1` and an estimate of `g_2` at local time `l_2`, `g_2`
    will be clamped such that
    `min_rate <= (g_2 - g_1)/(l_2 - l_1) <= max_rate`.

    It is unwise to enter slew mode until the clock is known to be at
    least reasonably accurate: otherwise it may take a very long time
    to catch up with a large future correction. For this reason, this
    function accepts a `maxerror` parameter which will cause it to
    return an error and remain in step mode if `(max-min)/2 >= maxerror`.

    Calling this function while already in slew mode is equivalent to
    switching to step mode and then immediately back into slew mode:
    it will cause the estimate to catch up to the current midpoint by
    a one-time step.

    A maximum rate of `INT64_MAX` is treated as infinity. A call of
    `byztime_slew(ctx, 0, INT64_MAX, maxerror)` will allow the estimate
    to advance at arbitrarily high or low speed but never to move
    backward.

    When in slew mode, it becomes possible for `byztime_get_offset()`
    and `byztime_get_global_time()` to return `(min,est,max)` tuples
    such that `est < min` or `est > max`. This can happen a previous
    estimate with wide error bounds is superceded by a new estimate
    with narrower ones which do not include the previous estimate.

    \param[in] ctx Pointer to context object.
    \param[in] min_rate_ppb Minimum clock rate in parts per billion.
    \param[in] max_rate_ppb Maximum clock rate in parts per billion.
    \param[in] maxerror Maximum error bound for slew mode to be
    allowed to take effect. May be `NULL`.

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.

    \exception ERANGE The current time is not known to within `maxerror`.
*/
int byztime_slew(byztime_ctx *ctx, int64_t min_rate_ppb, int64_t max_rate_ppb,
                 byztime_stamp const *maxerror);

/** Stop slewing time estimates.

    This function puts time estimation back into step mode after a previous
    call to `byztime_slew()`.

    \param[in] ctx Pointer to context object.

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.
*/
int byztime_step(byztime_ctx *ctx);

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
/** Install a signal handler for graceful recovery from page faults in the
   timedata file.

    If the timedata file gets truncated after it has been opened,
    future accesses to it will raise SIGBUS. This function installs a
    signal handler that will allow whatever function was trying to
    access the truncated file to gracefully error out with EPROTO
    rather than crashing the program. If the SIGBUS was caused for any
    other reason, this handler will reraise SIGBUS with the kernel
    default signal handler, which will cause the program to crash and
    dump core just as it normally would.

    A timedata file getting truncated while open is not something that
    should ever ordinarily happen; it would indicate that the byztime
    daemon or some or other process that has write permissions to the
    file is buggy or malicious. Benign mistakes such as the user
    specifying a path that does not point to a valid timedata file are
    detected and handled without relying on SIGBUS. Nonetheless,
    libbyztime is designed such that even a malicious byztime provider
    should not ever be able to cause a consumer to crash or hang, and it
    is necessary to be able to trap and recover from SIGBUS in order
    to uphold that guarantee.

    Calling this function will replace whatever SIGBUS handler was
    previously installed, so use it only if nothing else in your
    program needs to handle SIGBUS. Otherwise, instead call
    `byztime_handle_sigbus()` from within your custom signal handler.

    \param[in] oact If non-NULL, the action previously associated with
    SIGBUS will be stored in the pointed location.

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.

    This function is declared conditionally upon `defined(_POSIX_C_SOURCE) &&
   _POSIX_C_SOURCE >= 199309L`.
*/
int byztime_install_sigbus_handler(struct sigaction *oact);

/** Handle SIGBUS signals caused by page faults in the timedata file.

    This function must be called only from within a signal handler. If
    `signo` is `SIGBUS` and the signal was raised when a libbyztime
    function attempted to access a truncated timedata file, this function
    will not return and will `siglongjmp()` into the sigjmp
    environment which that function set up. Otherwise, it will return
    without doing anything.

    This function is declared conditionally upon `defined(_POSIX_C_SOURCE) &&
   _POSIX_C_SOURCE >= 199309L`.
*/
void byztime_handle_sigbus(int signo, siginfo_t *info, void *context);
#endif

/** @} */
/** \defgroup provider Provider API
    @{
*/

/** Opens a timedata file for read/write access, initializing it if necessary.

    \param[in] pathname The path to the timedata file.

    \return A pointer to a newly-allocated context object, or `NULL` on failure
    and sets `errno`.
*/
byztime_ctx *byztime_open_rw(char const *pathname);

/** Sets the time offset `(global time - local time)` and error bound.

    \param[in] ctx Pointer to context object.
    \param[in] offset The offset.
    \param[in] error Maximum error bound on `offset`.
    \param[in] as_of Local time as of which `error` was computed. If `NULL`,
    treated as "now".

    \returns 0 on success.
    \returns -1 on failure and sets `errno`.
*/
int byztime_set_offset(byztime_ctx *ctx, byztime_stamp const *offset,
                       byztime_stamp const *error, byztime_stamp const *as_of);

/** Gets the offset without any slewing or error calculation. */
void byztime_get_offset_quick(byztime_ctx const *ctx, byztime_stamp *offset);

/** Gets the data that was stored by the last call to `byztime_set_offset`,
   without any recomputation of the error bounds. */
void byztime_get_offset_raw(byztime_ctx const *ctx, byztime_stamp *offset,
                            byztime_stamp *error, byztime_stamp *as_of);

/** Recompute and record the difference between global time and real time.

    This is used to recover a best-guess `(global_time - local_time)` offset
    after the next reboot.

    \param[in] ctx Pointer to the context object.
    \returns 0 on success.
    \returns -1 on failure and sets `errno`.
*/
int byztime_update_real_offset(byztime_ctx *ctx);

/** @} */

#endif
