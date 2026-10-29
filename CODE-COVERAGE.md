# Code coverage of tapdisk with xcp-ng-tests

How the coverage of blktap (tapdisk, tap-ctl, tapback, libvhd) was measured on an XCP-ng 8.3 host
while running xcp-ng-tests. Scripts are in `scripts/`.

## Toolchain

- Build with devtoolset-11 (gcc 11.2.1) on the host. The lcov of XCP-ng does not work with this
  toolchain, but its gcov does: `/opt/rh/devtoolset-11/root/usr/bin/gcov`.
- Reports are made locally with lcov 2.0 and `gcov-11` (same `.gcda` format as gcc 11.2.1).

## Build and install (on the host)

This assumes the binaries of the blktap RPM have already been replaced by symlinks into `/opt/blktap`:
`/usr/bin/tapback`, `/usr/sbin/tap-ctl` and `/usr/local/libexec/tapdisk` point to
`/opt/blktap/{bin,sbin,libexec}/...`, and tapdisk loads `libvhd`/`libqcow2` from `/opt/blktap/lib`.

1. Export the branch into the build tree, e.g. `/root/cov/bt-cov` (`git archive`, copied to the host).
   The scripts take it from `$COV_TREE` (default `/root/cov/bt-cov`).
2. `scripts/build-cov.sh [tree [dest]]`: configure with
   `CFLAGS="-O0 -g --coverage -fprofile-update=atomic" LDFLAGS=--coverage --prefix=/opt/blktap`,
   build, install into `/opt/blktap-cov`.
   `-fprofile-update=atomic` because tapdisk counters are updated by several IO threads.
3. Back up the current install (`tar cf /root/cov/opt-blktap-before.tar /opt/blktap`, plus the
   targets of `/usr/bin/tapback`, `/usr/sbin/tap-ctl`, `/usr/local/libexec/tapdisk`), then copy
   `/opt/blktap-cov` onto `/opt/blktap` and restart tapback.

The `.gcda` files are written next to the `.gcno` files in the build tree, at the normal exit of
each process, and accumulate across runs. Delete them to start a new campaign.

## Run the tests

From xcp-ng-tests, against the host and a test VM:

```
uv run --frozen pytest -v --maxfail=100 <tests> --hosts=<host> --vm=<vm-uuid>
```

Before collecting, every instrumented process must have exited:

- tapdisk: shut down the VMs / disconnect the VBDs;
- tap-ctl: exits after each command;
- tapback: per-domain slaves exit when their domain is destroyed. The master exits with `_exit()`
  in its SIGTERM handler (`tapback/tapback.c`), so it never writes its counters; this would need
  a call to `__gcov_dump()` before `_exit()` in a coverage build, or gdb.

## Collect and report

- lcov (preferred), locally, three scripts:
  - `scripts/lcov-report.sh <host> <outdir>` fetches the build tree (sources, `.gcno`, `.gcda`, ~32 MB)
    and runs `lcov --capture --gcov-tool gcov-11` into `<outdir>/cov.info`, with no filter. Paths are
    rewritten with `--substitute`; `/usr/*` is left out because the system headers of dom0 differ from
    the local ones.
  - `scripts/lcov-filter.sh [-f rules] [-e rule]... in.info out.info` removes files, functions, line
    ranges or lines matching a regexp from a tracefile (syntax in `scripts/lcov-filter.py`; the filters
    we use are in `scripts/cov-filters.conf`: `qcow2/` and `include/` (imported code), `conftest.c`, and the dead
    or development-only code: `block-ram/valve/cache/lcache/llcache.c`, `tapdisk_snprintf`).
  - `scripts/lcov-filtered-report.sh <host> <outdir> [title]` does both, then genhtml:
    `cov.info` (raw), `cov-filtered.info`, `html/`. `FILTERS=<file>` replaces the rules file
    (`/dev/null` for none), `FILTER_RULES='function ^foo$; lines drivers/x.c 10-20'` adds rules.
  Campaigns can be combined with `lcov -a a.info -a b.info`.
- Without a local gcov-11: `scripts/collect-cov.sh <label>` on the host runs gcov in JSON mode
  (`gcov -j -b`) for every object; then locally `scripts/cov-merge.py out-<label>` merges them
  per source file into `cov.json`, and `scripts/cov-report.py cov.json` prints a summary per
  component and per file. Both methods give the same number of executed lines.

## Campaigns

All on 10.1.30.2 (build 19e2dff9, `-O0`), counters accumulated in the same tree; the first campaign was
archived (`/root/cov/gcda-quick.tar`) and its counters deleted before the next ones.

| campaign | tests | log |
|---|---|---|
| quick | `blktap`: `test_vbd_connector`, `test_tapdisk_vm_io`, `test_multiqueue`, `test_nbd` (28 tests, ~11 min) | `t1.log`, `t2.log` |
| large | `test_single_tapdisk`, `test_xapi_nbd` (skipped: xapi < 26.1.21), `snapshot/test_snapshot_ignore_vdis`, `misc/test_basic_without_ssh`, `misc/test_vm_basic_operations` (no live migrate): 15 passed, 3 skipped, 20 min | `t3.log` |
| migration | `test_live_migration.py -k cross_pool`, 10.1.30.2 ↔ 10.1.30.3 (the other host runs an ASAN build, not measured): 14.5 min | `t4.log` |
| fio | `test_fio_bench.py`, small and short (see below) | `t5.log`, `t6.log` |
| gcov branch | the tests of `eva/blktap-gcov` (tap-ctl CLI, signals, `ram:`, qcow2 commit, CBT, `vhd-util`, nbd): 65 passed, 3 failed (CBT tests: busybox `dd` has no `oflag=direct`) | `t7.log` |

Logs and reports: `~/.cache/metal-bench/lab-10.1.30.2/2026-10-29-gcov-kd2/`.

### Results (filtered by `scripts/cov-filters.conf`)

| | functions | lines | branches |
|---|---|---|---|
| quick (filter: `qcow2/`, `include/`) | 52.1 % | 36.6 % | 25.4 % |
| quick + large + migration (same, plus `tapdisk_snprintf`) | 54.1 % | 38.5 % | 26.7 % |
| everything, current filters (1059 / 1335 functions) | 79.3 % | 58.7 % (13093 / 22298) | 42.0 % |

The filters grew between the rows (the last one also leaves out the development and dead drivers), so the
denominators differ: compare the steps, not the exact figures.

The big steps: the cross-pool migration (`block-nbd.c` 2 % → 53 %), the `vhd-util` tests (`libvhd.c` 24 % →
59 % of the lines, `libvhd-journal.c` 0 → 56 %), the `tap-ctl` tests (`tap-ctl.c` 98 % of the functions), CBT
(`block-log.c` 91 %), the fio benchmark (`block-aio.c` 90 %). What is left, with the tasks to close it, is in
`CODE-COVERAGE-TESTS.md` (checklist).

### How the remaining gap was estimated

Per file, the uncovered lines of `drivers/` were split with a heuristic into error paths (blocks that return
an error, `goto fail`, log `ERR`/`WARN`, `td_panic`...) and the rest. Leaving the error paths out, the line
coverage of the main files is 75 to 95 %: `block-vhd.c` 77 → 91 %, `tapdisk-control.c` 66 → 91 %,
`tapdisk-vbd.c` 66 → 89 %, `tapdisk-nbdserver.c` 45 → 84 %, `block-nbd.c` 53 → 85 %. The real gaps are the NBD
options and block status, the teardown under I/O, encrypted VHDs and a few isolated blocks. The heuristic is
rough: read the code of a block before deciding that it is an error path.

## Pitfalls found

- **A test that calls a tool by name runs the RPM binary**, which has no counters. `cbt/cbt-util.c` had none
  because `test_blktap_cov_cbt.py` ran the bare `cbt-util`. Tests must use
  `blktap_tool(host, name)` (`tests/storage/blktap/__init__.py`: `/opt/blktap/{bin,sbin}` first, else the
  PATH). Same for `vhd-util`, `td-util`, `lvm-util`, `tap-ctl`.
- **Files never loaded are missing from lcov**: without a `.gcda` a file is not in the report, whatever its
  size (`td.c`, `td-rated.c`, `posixaio-backend.c`, `tapdisk-stream.c`, `cbt-util.c`, `cpumond.c`: about 180
  functions), so the percentages above are optimistic (about 48 % of the functions before the last
  campaigns, not 54 %). An `lcov --capture --initial` baseline from the `.gcno` would fix it (todo).
- **Request merging in `io-optimize.c`** (`expand_iocb`, `io_expand_iocbs`) and `fail_tiocbs` run only after an
  `io_submit` failure: they are error paths, not something a load test reaches.
- **Imported or dead code distorts the totals**: `qcow2/` (QEMU), `include/` and the legacy drivers are
  filtered; `block-vindex.c` (414 lines) and the `libvhd-index` / `vhd-index` stack are not yet.
- **The hosts**: `/` of 10.1.30.2 is ext3, so `fallocate` fails there (raw file images of the fio benchmark,
  only the LV variant runs); the guest is Alpine (busybox tools).

## Limits

- `-O0` changes timings (the deferred kick is sensitive to them).
- The SR test suites (`tests/storage/ext`, `lvm`, ...) need a disk they can erase.
- SM calls the `vhd-util` of the RPM (`/usr/bin/vhd-util`, linked to `/lib64/libvhd.so.0`), which
  is not instrumented: snapshot/coalesce done by SM do not show up in `vhd/`.

## Restore the host

Restore `/opt/blktap` from `/root/cov/opt-blktap-before.tar`, check the links listed above,
restart tapback and compare the md5 of tapdisk with the one saved before.

## Campaign note: the fio benchmark

`test_fio_bench.py` belongs in the coverage campaigns, with a small image and short runs
(`--blktap bench-matrix:short,bench-runtime:5,bench-image-size:512`; once more with
`-k raw --blktap bench-image-vg:<vg>` for a raw image on a LV). It is the only test that writes through
the raw (`aio:`) driver, so `block-aio.c` needs no test of its own; see task 1.2c of
`CODE-COVERAGE-TESTS.md`. About 4 minutes per format: clone and boot of the VM (~45 s), prefill, 12 runs of
12 s (5 s of measure + 5 s of ramp), teardown.
