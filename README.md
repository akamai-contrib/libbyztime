# libbyztime

This library provides an interface between
[byztimed](https://github.com/akamai-contrib/byztimed)
and applications which consume time from it.

## Portability

The interface to this library is portable, with the minor caveat that
some of the error numbers it returns, such as `EOVERFLOW`, are not
part of C17. They are, however, part of both POSIX and C++11 (through
`<cerrno>`) and therefore available virtually everywhere.

This implementation, however, is very much Linux-specific. In
particular, it depends on `/proc/sys/kernel/random/boot_id` and on
`CLOCK_MONOTONIC_RAW`.

Although only tested on AMD64, it should be portable to most
architectures.  It needs the ability to atomically update a 32-bit
word without needing a mutex, but this ability exists nearly
everywhere. It checks for it via the `ATOMIC_INT_LOCK_FREE` macro,
so if you try to target an architecture where you can't do this
you'll get a compile-time error.

We depend on the `__builtin_{add,sub,mul}_overflow()` functions,
which are GCC extensions but also available in Clang.

## Build

Just run `make`. This library has no build dependencies other than
libc and a compiler toolchain.

This repository does not contain any tests. The unit tests for this
library are part of the byztimed repo. (This way we get simultaneous
test coverage of libbyztime and its Rust bindings).

## Usage

For complete API documentation, see `byztime.h` or run Doxygen via
`make doc`. The only functions that simple clients need to understand
are `byztime_open_ro()` and `byztime_close()` to open and close
communication with byztimed, and `byztime_get_global_time()` to get
the time; many will also need `byztime_slew()`.

## Known bugs

`byztime_get_clock_era()` is supposed to change its result whenever an
event occurs which renders two `byztime_get_local_time()` results
incomparable.  In this implementation, `byztime_get_clock_era()` the
boot ID, and `byztime_get_local_time()` returns
`CLOCK_MONOTONIC_RAW`. Therefore, reboots correctly result in an era
change. However, `CLOCK_MONOTONIC_RAW` can also be disrupted by a
suspend-to-RAM (ACPI S3 sleep), yet this will not alter the boot ID.
