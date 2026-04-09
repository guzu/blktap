# tapdisk-vbd-stress

Standalone micro-benchmark / stress harness for the tapdisk VBD I/O hot
path. It wires a synthetic "blkif" producer to a real `td_vbd_t` through
the real tapdisk scheduler, with a dummy block driver underneath that
completes every `td_request_t` synchronously.

The goal is to stress the VBD request issue path and the per-VBD mutex
(`td_vbd_handle.mutex`) at high request rates so that future
multi-threaded producers can be added on top with confidence.

This is **not** a cmocka mockatest: it links against the real
`libtapdisk.la` and exercises the same code paths the in-tree tapdisk
binary uses. It is a `noinst_PROGRAMS` target — built but never
installed.

## Build

The harness is built as part of the normal blktap build:

```sh
./autogen.sh        # only the first time
./configure
make
```

To rebuild only the harness:

```sh
cd stress
make
```

The binary lands at `stress/tapdisk-vbd-stress`.

For meaningful results when investigating lock contention or scheduler
behavior, build the whole tree with `-O0 -g3` (no inlining of
`libtapdisk.a` symbols, usable backtraces in gdb).

## Run

```sh
./tapdisk-vbd-stress [options]
```

By default it runs for 30 s against a 512 MB virtual disk, submitting
mixed read/write requests as fast as the scheduler will accept them,
with no cap on the total number of requests. It then prints a summary:

```
stress: stop reason: target reached
stress: target=100000 submitted=100000 completed=100000 errors=0
stress: 11.234s elapsed, 8902 req/s
```

The stop reason is one of `target reached`, `max runtime reached` or
`drained`. The drive loop always waits for in-flight requests to
complete before reporting, so `submitted == completed` should always
hold; if it does not, that is a bug.

## Options

| Flag   | Default | Description |
|--------|---------|-------------|
| `-n N` | 0       | Total requests to submit. `0` = unlimited. **Setting `-n` implicitly sets `-T 0`** unless `-T` is also given explicitly — i.e. asking for a fixed request count switches the run from "time-bounded" to "count-bounded". |
| `-d N` | 32      | Max in-flight requests at any time. |
| `-s N` | 8       | Sectors per iov. |
| `-m M` | `x`     | Operation mix: `r` = read-only, `w` = write-only, `x` = mixed. |
| `-b N` | 32      | Max submissions per producer tick (random in `[1, N]`). Drives how bursty the producer is, which controls how long `vbd->mutex` is held in `tapdisk_vbd_issue_request`. |
| `-l N` | 100     | Max injected per-request latency in microseconds (random in `[0, N]`). Applied inside the dummy driver, before `td_complete_request`. Set to `0` to disable. |
| `-T N` | 30      | Hard wall-clock cap in seconds. `0` = unlimited. The producer stops enqueuing when the cap is reached and the loop drains the in-flight queue before reporting. Implicitly disabled when `-n` is given alone. |
| `-i N` | 4       | Max iovs per vreq, random in `[1, N]`. Hard max 32. Real blkif tops out at 11 (`BLKIF_MAX_SEGMENTS_PER_REQUEST`); the harness leaves some headroom so the issue loop can be deliberately over-subscribed. Each iov carries `-s` sectors, so total sectors per request is `iovcnt * secs`. |
| `-v`   | off     | Verbose. |
| `-h`   |         | Usage. |

## Example invocations

```sh
# Smoke test: 5000 reads, single-iov, no latency.
./tapdisk-vbd-stress -n 5000 -m r -i 1 -l 0

# Lock-contention probe: large bursts, large iovcnt, no latency in
# the dummy driver, time-bounded.
./tapdisk-vbd-stress -n 100000000 -T 5 -b 32 -i 32 -l 0

# Realistic blkif sizing: up to 11 segments per vreq, mixed RW.
./tapdisk-vbd-stress -n 200000 -i 11
```

## Running under valgrind

Valgrind catches uninitialized reads, leaks, and bad frees end-to-end.
No special build flags are required, but `-O0 -g3` (already the default
in this tree) gives readable stack traces.

### Run it through libtool, not directly

When built in-tree, `stress/tapdisk-vbd-stress` is **not** the real ELF
— it is a shell script that libtool generates as a wrapper, so that the
in-build-tree `.so`s in `vhd/lib/.libs`, `qcow2/lib/.libs`, etc. are
found via `LD_LIBRARY_PATH` instead of needing `make install`. The real
binary lives at `stress/.libs/tapdisk-vbd-stress`.

If you run valgrind on the wrapper, it will valgrind `/bin/bash`, then
`exec` into the real binary, lose its instrumentation context, and
report a pile of useless errors (or none). Always go through libtool's
`--mode=execute`, which sets up the env and then `exec`s valgrind on
the real ELF:

```sh
cd stress
../libtool --mode=execute \
    valgrind \
        --leak-check=full \
        --show-leak-kinds=all \
        --track-origins=yes \
        --error-exitcode=1 \
        ./tapdisk-vbd-stress -n 2000 -T 60 -l 0
```

Same trick works for `gdb`, `strace`, `perf record`, etc. — anything
that needs to see the real ELF and its `LD_LIBRARY_PATH`.

A quick way to verify you're not accidentally launching the wrapper:

```sh
file ./tapdisk-vbd-stress         # → "POSIX shell script" (the wrapper)
file ./.libs/tapdisk-vbd-stress   # → "ELF 64-bit LSB executable"
```

### Notes

- Drop `-l` to `0` to avoid the dummy driver's `usleep()`, which makes
  valgrind feel even slower than its baseline ~20x.
- A handful of "still reachable" blocks belonging to the tapdisk
  scheduler / metrics shm are expected at exit since the harness does
  not call a full teardown — only "definitely lost" matters.
- Once `make install`ed, the wrapper goes away and the installed
  binary is the real ELF, so plain `valgrind ./tapdisk-vbd-stress`
  works. But the harness is `noinst_PROGRAMS` and not installed, so in
  practice you always need libtool here.

## Running under AddressSanitizer (ASan)

ASan catches use-after-free, heap/stack overflows, and double frees
much faster than valgrind. It requires rebuilding the **whole tree**
(not just the harness) with the sanitizer flags so that
`libtapdisk.a` is instrumented too:

```sh
make distclean
./configure CFLAGS="-O0 -g3 -fno-omit-frame-pointer -fsanitize=address" \
            LDFLAGS="-fsanitize=address"
make
cd stress
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
    ./tapdisk-vbd-stress -n 5000 -T 30
```

Notes:
- `-fno-omit-frame-pointer` keeps backtraces accurate.
- ASan is incompatible with TSan in the same build — pick one.
- If a third-party `.so` (libvhd, libqcow2) is linked uninstrumented,
  ASan will still catch issues *inside* the tapdisk code paths the
  harness exercises; only errors that originate from the uninstrumented
  libs will be missed.

## Running under ThreadSanitizer (TSan)

TSan is the relevant tool once a multi-threaded producer is added on
top of this harness (see `TODO.md` §5). The single-threaded mode is
still useful as a baseline: TSan should report **zero** races on the
in-tree harness, and any race that appears as soon as a second
producer thread is wired in is a real bug in the VBD path.

Same drill as ASan — rebuild the full tree:

```sh
make distclean
./configure CFLAGS="-O1 -g3 -fno-omit-frame-pointer -fsanitize=thread" \
            LDFLAGS="-fsanitize=thread"
make
cd stress
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
    ./tapdisk-vbd-stress -n 20000 -T 30
```

Notes:
- TSan needs `-O1` or higher to keep instrumentation overhead
  reasonable; `-O0` works but is painfully slow.
- TSan and ASan cannot be combined. Run them in separate builds.
- TSan is sensitive to `LD_PRELOAD` and to non-instrumented libraries
  loaded via `dlopen` — if you see opaque "race in libc" reports,
  rebuild glibc-using deps with the sanitizer or suppress with
  `TSAN_OPTIONS=suppressions=…`.

## Architecture in one paragraph

`main()` calls `tapdisk_server_init()` + `tapdisk_server_complete()` to
bring up the real scheduler and AIO backends, then `setup_vbd()`
allocates a `td_vbd_t` via `tapdisk_vbd_initialize()`, attaches a
hand-rolled `td_driver_t` (no `tapdisk_driver_allocate()` because there
is no registered DISK_TYPE), and points `vbd->vdi_stats.stats` at a
heap-allocated `struct stats` so the metric increment macros do not
NULL-deref. The producer is a 0-timeout scheduler event that picks a
random batch size, builds a `td_vbd_request_t` with a random number of
iovs, and calls `tapdisk_vbd_queue_request()`. The dummy driver
synchronously completes every `td_request_t` (with optional injected
latency). The drive loop calls `tapdisk_server_iterate()` directly
instead of `tapdisk_server_run()` to avoid pulling in signalfd.

## TODO

See [TODO.md](TODO.md) for the planned extensions (latency
histograms, multi-VBD, deferred completion, error injection,
multi-producer for real lock-contention measurements, etc.).
