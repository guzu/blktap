# Checklist: tests to add to raise the code coverage of blktap

For the agent that writes the tests. Method and results: `CODE-COVERAGE.md`.
Priority is `drivers/` (`tapdisk-*`, `td-*`, `block-*`, `*-backend`), then `control/`, `tapback/`,
`vhd/`, `cbt/`, `lvm/`.

Legend: `[x]` done and seen in the report, `[~]` partly done, `[ ]` to do, `[-]` dropped (reason given).
Tick a box only after looking at the lcov output, never from the test result alone.

## Where we are (cumulative report, filtered by `scripts/cov-filters.conf`)

Host 10.1.30.2, build 19e2dff9, campaigns: quick, large + cross-pool migration, `test_fio_bench.py`, branch
`eva/blktap-gcov` (65 of 68 passed). Without `qcow2/`, `include/`, `block-ram/valve/cache/lcache/llcache`:

| | covered | total | % |
|---|---|---|---|
| functions | 1059 | 1335 | 79.3 % |
| lines | 13093 | 22298 | 58.7 % |
| branches | 5072 | 12070 | 42.0 % |

Line coverage of `drivers/` once the error paths are ignored (heuristic: blocks that return an error,
`goto fail`, log `ERR`/`WARN`, `td_panic`... are left out of both counts):

| file | lines now | error paths left out | main uncovered non-error code (lines) |
|---|---|---|---|
| `tapdisk-nbdserver.c` | 44.7 % | 84 % | option negotiation (`receive_newstyle_options` 88), `listen_inet` 85, INFO/GO, structured replies, block status, `free_extents` |
| `block-nbd.c` | 53.2 % | 85 % | new-style negotiation and import session, `__cancel_req`, `tdnbd_pending`, "connection is dead" close |
| `tapdisk-vbd.c` | 65.6 % | 89 % | read past the parent's end (12), `block_status_add_extent`, `kill_queue`, `drop_log`, `close_vdi` |
| `tapdisk-server.c` | 67.4 % | 86 % | SIGUSR2, reaper wake/event, `lowmem_*`, `cpumond_*` |
| `tapdisk-control.c` | 66.1 % | 91 % | `close_connection`, stats of all VBDs, pending connections, `cancel_commit_job` |
| `block-vhd.c` | 76.6 % | 91 % | encrypted VHD (`__load_crypto` 27, `switch (r->op)`), bitmap LRU wrap and cache flush, `vhd_queue_block_status` |
| `block-qcow2.c` | 78.8 % | 91 % | `do_cancel_commit_job`, thread join at close, `min`/`max` of the statistics |
| `libaio-backend.c` | 64.4 % | 82 % | `defer_tiocb` / `queue_deferred_tiocb` (queue full), `cancel_all_tiocbs` |
| `tapdisk-syslog.c` | 52.9 % | 75 % | ring write / read / dispatch, socket event |
| `td-ctx.c` | 71.4 % | 84 % | `shared_ctx_close` (19), 32-bit ring protocol (9) |
| `tapdisk-loglimit.c` | 20.6 % | 21 % | the whole rate limiter |
| `tapdisk-image.c` | 42.0 % | 90 % | `open_x_chain` (110), `find_parent_nbd_path` (41), `parse_flags` (35) |
| `td-req.c`, `td-blkif.c`, `scheduler.c`, `tapdisk-utils.c` | 72-82 % | 77-96 % | small gaps (`scheduler_get_event_uuid`, except fds, `tapdisk_get_image_size`, `tapdisk_syslog_facility_by_name`) |
| `block-vindex.c` | 0 % | 0 % | all of it (36 functions, 414 lines): see 1.9 |

Most of the remaining gap is therefore NBD, a few teardown / hot-unplug paths, and encrypted images.

## Where and how

- Tests: `/home/manu/SRC/vates/xcp-ng-tests`, branch `eva/blktap-gcov` (on top of `eva/blktap-tests`),
  directory `tests/storage/blktap/`. Do not modify `tests/migration/`. Commit only if the user asks.
- Reuse the helpers: `lib/blktap.py` (`TapCtl`, `VBDConnector`, `XenStoreHelper`, `blktap_tool()`),
  fixtures of `tests/storage/blktap/conftest.py` (`tapctl`, `vbd_connector`, `xenstore`, `image`/`images`),
  helpers of `tests/storage/blktap/__init__.py`, and the NBD client script of `test_nbd.py` (python3 run in
  dom0 on `/var/run/blktap-control/nbd<pid>.<minor>`): the way to produce I/O on a tapdisk **without a VM**.
  The `test_blktap_cov_*.py` files of the branch show the style.
- `@pytest.mark.no_vm` for tests without a VM; `small_vm` + `unix_vm` for the others.
- Run: `uv run --frozen pytest tests/storage/blktap/<file> -v --hosts=10.1.30.2 --vm=02145eb7-e49c-0fda-a403-8079883dc844`.
  The hosts are shared lab machines: ask the user before changing anything on them; never leave tapdisks,
  VBDs, clones or CBT-enabled VDIs behind.
- The guest is Alpine (busybox): no `dd oflag=direct`; use fio or install `coreutils`.
- A test must also pass on a host without the instrumented build (`blktap_tool()` falls back to the PATH).

## How to check that a test did its job

The instrumented build is on 10.1.30.2 (`/opt/blktap` → build, sources and counters in `/root/cov/bt-cov`).
Ask the user before using it.

1. Delete the counters: `find /root/cov/bt-cov -name '*.gcda' -delete` (all tapdisks stopped).
2. Run the new tests; every tapdisk must exit normally (counters are written at `exit`).
3. `COV_TREE=/root/cov/bt-cov scripts/lcov-filtered-report.sh 10.1.30.2 <outdir>`, then check the hit
   counts of the target functions and the lines named in the task (`html/` or the `FNDA:` / `DA:` records).
4. A process ended by `_exit()` or a signal writes no counters (tapback master...).

---

# Part 0: measurement

- [x] Filters separate from the capture: `scripts/lcov-report.sh`, `lcov-filter.sh`, `lcov-filtered-report.sh`,
  `cov-filters.conf`.
- [ ] Add an `lcov --capture --initial` baseline (from the `.gcno`) combined with `-a` in
  `scripts/lcov-report.sh`: files never loaded (`td.c`, `td-rated.c`, `posixaio-backend.c`,
  `tapdisk-stream.c`, `cbt/cbt-util.c`, `cpumond.c`, `vhd-index.c`...: about 180 functions) are missing from
  every total. Do it first.
- [ ] Ask the user which of the dead-code candidates of 1.9 go into `cov-filters.conf`.

---

# Part 1: drivers/

## 1.1 Signals sent to a tapdisk (`tapdisk_server_signal_handler`)

Done in `test_blktap_cov_signals.py`: SIGUSR1 and SIGHUP.

- [x] SIGUSR1 → `tapdisk_server_debug` and the `*_debug` of the open VBD.
- [x] SIGHUP → `tlog_reopen_cb`.
- [ ] SIGUSR2 → `tapdisk_start_polling_all_queues` (`td-blkif.c`, 10 lines) and the `case SIGUSR2` of the handler
  (`tapdisk-server.c:713`): needs a tapdisk connected to a VM (`vbd_connector`); send the signal, assert
  `write_then_read` still works.
- [ ] SIGXFSZ → `tapdisk_vbd_kill_queue` (`tapdisk-vbd.c:1121`): assert the tapdisk survives and the VBD fails
  its requests cleanly. Use a VBD with no VM.

## 1.2 `tap-ctl open` options

- [x] `-C <log>` (write logger = CBT, `block-log.c` 91 % of its functions), through the no-VM test.
- [~] `-2 <path>` secondary / mirror: `tapdisk_vbd_add_secondary` is hit by the storage migration, but 22 of its
  41 lines are not (the non-mirror secondary, `-s` standby). A no-VM test with `-2` and a write through the NBD
  client, then compare both images, and one with `-s`.
- [ ] `-e <minor>` (stack on the parent chain of another tapdisk): `find_parent_nbd_path` (41 lines) and the
  `prt_devnum` branches of `__tapdisk_image_open_chain`. Open an image on tapdisk A, `-e` it from tapdisk B.
  Needs `-e` in `TapCtl.open()`.
- [ ] Image descriptor with flags and `x-chain`: `tapdisk_image_parse_flags` (35 lines) and
  `tapdisk_image_open_x_chain` (110 lines, `tapdisk-image.c:435`). Read `__tapdisk_image_open_chain` for the
  syntax (`type:path` with args, or `x-chain:`) and open one such descriptor. If it is only legacy (blktap2
  kernel chains), tick `[-]` and say so.
- [-] `-r` read caching: `block-lcache.c` / `block-llcache.c` are in `cov-filters.conf` (dead code).

## 1.2b Changed block tracking = `block-log.c`

`block-log.c` is the write logger behind CBT (SM passes the log file with `-C`).

- [x] No-VM tests (`test_log_marks_the_written_blocks`, `test_log_with_many_writes`): `tap-ctl open -C`, writes through
  the NBD client, bitmap checked.
- [x] Pause/unpause with and without the log (`test_log_follows_pause_and_unpause`).
- [ ] **The `cbt-util` tests run the RPM binary**: `test_blktap_cov_cbt.py` calls the bare `cbt-util` (line 40), so
  `cbt/cbt-util.c` has no counters although `test_cbt_util_fields` and `test_cbt_util_coalesce` pass. Use
  `blktap_tool(host, 'cbt-util')` (it looks in `/opt/blktap/{bin,sbin}`, it already exists in `__init__.py`).
  Same check for every other test that calls `vhd-util`, `td-util`, `lvm-util` or `tap-ctl` by name.
- [ ] Fix the three VM tests (`test_vm_writes_are_logged[vhd|qcow2]`, `test_xapi_cbt`): the guest `dd` has no
  `oflag=direct`. Use fio (`--direct=1`) or install `coreutils`.
- [ ] Snapshot, then write again: the new log holds only the second set of writes (`cbt-util get -p -c`).
- [ ] `tapdisk_vbd_drop_log` (watchdog with pending requests idle, `tapdisk-vbd.c:1470`) — only if a stuck-request
  scenario is cheap; else skip and say so.

## 1.2c Raw images (`aio:`, `block-aio.c`)

- [x] No test of its own: `test_fio_bench.py` (qcow2, vhd, raw on a LV) covers the write path
  (`block-aio.c` 90 % of functions). Run it in every campaign, small and short:
  `--blktap bench-matrix:short,bench-runtime:5,bench-image-size:512,bench-image-vg:<vg>`.
- [-] `tdaio_validate_parent` is `return -EINVAL;`: nothing to check.
- [-] Raw on a file: `fallocate` is not supported on the ext3 root of 10.1.30.2; the LV replaces it.

## 1.3 `ram:` image driver

- [x] `test_ram_image` (now filtered out by `cov-filters.conf`: development driver).

## 1.4 Commit jobs

- [x] `tap-ctl commit` + `query` on a qcow2 with a backing file (`test_qcow2_commit`): `tap-ctl-commit.c` 66 %.
- [ ] `tap-ctl cancel` of a running job: `tapdisk_control_cancel_commit_job` (27 lines), `td_cancel_commit_job`
  (23), `tapdisk_vbd_cancel_commit_job` (14), `do_cancel_commit_job` (15, `block-qcow2.c:1436`). The job must
  still be running: use a large enough image (write a lot of clusters before the commit).
- [ ] `tapdisk_control_close_connection` (`tapdisk-control.c:453`): kill a `tap-ctl` client in the middle of a
  command (a long commit query, or `kill -STOP` then `-KILL`).

## 1.5 NBD server and client — the largest gap

Extend the NBD client script of `test_nbd.py` (spec: `doc/proto.md` of the NBD project). One scenario each:

- [ ] `NBD_OPT_INFO` / `NBD_OPT_GO` (with and without block size), `NBD_OPT_LIST`, known and unknown export
  name: `send_nbd_opt_infogo` (65 lines), `send_info_export`, `send_info_block_size`, `send_nbd_rep_server`,
  `send_fixed_option_reply`, and most of `receive_newstyle_options` (88 lines, `tapdisk-nbdserver.c:635`).
- [ ] `NBD_OPT_STRUCTURED_REPLY` then structured `NBD_CMD_READ`, including a read past the end:
  `__tapdisk_nbdserver_structured_read_cb` (64), `__tapdisk_nbdserver_send_structured_reply`,
  `send_structured_reply_error`.
- [ ] `NBD_OPT_SET_META_CONTEXT` `base:allocation` + `NBD_CMD_BLOCK_STATUS`, with and without
  `NBD_CMD_FLAG_REQ_ONE`, on a VHD (and a qcow2 if it supports it) with a known allocation pattern; check the
  extents: `send_meta_context`, `send_structured_reply_block_status` (49), `convert_extents_to_block_descriptors`,
  `free_extents`, `__tapdisk_nbdserver_block_status_cb`, and the image side `td_queue_block_status`,
  `vhd_queue_block_status`, `tapdisk_vbd_complete_block_status_request`, `add_extent`, `block_status_add_extent`,
  the `TD_OP_BLOCK_STATUS` case of `tapdisk_vbd_complete_td_request_cb`.
- [ ] `block-nbd.c` as a new-style client: a second tapdisk opening `nbd:<unix socket>` of the first one's
  server: `tdnbd_nbd_negotiate_new`, `tdnbd_connect_import_session`, `negotiate_client_newstyle_options`
  (and the `NBD_FIXED_SINGLE_EXPORT` branch, line 564).
- [ ] `block-nbd.c` close paths: `__cancel_req`, `tdnbd_pending`, the "connection is dead" close (line 1049):
  close the tapdisk while requests are in flight (model: `test_close_with_nbd_reads_in_flight`).
- [ ] Listening: `tapdisk_nbdserver_listen_inet` (85 lines), `tapdisk_nbdserver_newclient`,
  `tapdisk_nbdserver_contains_client`, `get_in_addr`: find out how the TCP listener is enabled (callers around
  `tapdisk-nbdserver.c:1789`); if it is a supported configuration add a test (connect from dom0 over TCP), else
  tick `[-]` and propose it for removal.

## 1.6 `tapdisk-vbd.c`: teardown and hot-unplug

- [ ] Disconnect a VBD while the guest runs fio (the current `test_hotunplug.py` is skipped; fix or replace it):
  `tapdisk_vbd_kill_requests` (18), `tapdisk_vbd_shutdown` (32), `tapdisk_vbd_close` (36),
  `tapdisk_vbd_close_vdi` (lines 939-944). Also the reaper: `tapdisk_server_reaper_wake` / `_event`
  (`tapdisk-server.c:397-427`, called from `td-req.c:722`: read the condition first, it is probably the IO thread
  asking the main thread to reap a closing VBD).
- [ ] Read past the end of the parent of a differencing image (`tapdisk-vbd.c:1673-1688`, 12 lines): a VHD or
  qcow2 child resized larger than its parent, then a request straddling the parent's end (`vhd-util resize` on
  the child + `snapshot`, then NBD read at the boundary).
- [-] `tapdisk_vbd_lock` (`return 0`), `tapdisk_vbd_initialize` (only `tapdisk-stream`), `signal_enospc` (error).

## 1.7 libaio backend

- [ ] Queue full: `defer_tiocb` / `queue_deferred_tiocb` (33 lines, `libaio-backend.c:117-145`): more requests in
  flight than the aio context holds (see `libaio_backend_queue_full`): a short fio run with a large iodepth and
  several jobs (for example 4k randread, iodepth 256, numjobs 4, 5 s) on a VM with 4 vCPUs.
- [ ] `libaio_backend_cancel_all_tiocbs` (11 lines): the same load, then close the tapdisk with requests queued
  (1.6).
- [-] `io_expand_iocbs`, `expand_iocb`, `fail_tiocbs`, `cancel_tiocbs`, `iocb_opcode`: they only run after an
  `io_submit` failure or in a debug print (error path); the request merging itself is already covered.
- [-] `__lio_setup_aio_poll`: fallback when the eventfd setup fails.

## 1.8 Xen ring side (`td-blkif.c`, `td-ctx.c`, `tapdisk-blktap.c`)

- [ ] Force-disconnect a frontend (`xe vm-shutdown force=true` with I/O in flight):
  `tapdisk_xenblkif_unmap_ring`, `tapdisk_xenio_shared_ctx_close` (`td-ctx.c:48`, 19 lines).
- [ ] 32-bit ring protocol (`BLKIF_PROTOCOL_X86_32`, `td-ctx.c:96`): only with an i386 guest; skip unless one
  exists, and say so.
- [-] `tapdisk_blktap_compat_create_device` (old kernel blktap, dead), `tapdisk_blktap_fail_request` (error).

## 1.9 Dead or legacy code: ask the user, do not test blindly

Not reachable from `tap-ctl` in this tree, or needing external daemons. For each, grep the callers and report;
if it is dead, propose it for `cov-filters.conf` (or removal) instead of testing it.

- [x] Already filtered by the user: `block-ram.c`, `block-valve.c`, `block-cache.c`, `block-lcache.c`,
  `block-llcache.c`.
- [ ] `block-vindex.c` (36 functions, 414 lines) with `libvhd-index.c` (43) and `vhd-index.c` (20): the
  `VHD_INDEX` flag has no `tap-ctl` option.
- [ ] `td-rated.c` (61), `posixaio-backend.c` (28), `tapdisk-stream.c` (24, `noinst`): invisible until the
  baseline of Part 0 is done.
- [ ] Encrypted images: `block-crypto.c` (6 uncovered functions), `crypto/xts_aes.*` (5), `__load_crypto` in
  `block-vhd.c` (27 lines), the encrypted `switch (r->op)` (8 lines). Testable: `vhd-util key` then
  `tap-ctl open -E` (key on stdin). Decide with the user: test or filter.
- [ ] `qcow2_validate_parent`, `tdnbd_validate_parent`, `tdnbd_get_parent_id` and the other `*_get_parent_id`:
  one-liners reached only when the driver is a parent; skip.

## 1.10 Logging, server maintenance (low priority)

- [ ] `tapdisk-syslog.c` ring (39 non-error lines): flood the tapdisk with log lines faster than the syslog
  socket drains them (`tapdisk_syslog_ring_*`, `tapdisk_syslog_sock_event`).
- [ ] `tapdisk-loglimit.c` (27 lines): needs a driver that logs the same message many times per second
  (`tapdisk_driver_log_pass` in `tapdisk-driver.c`); find one cheap trigger, else skip.
- [ ] `tapdisk2.c` `main` options (`-D`, `-h`, `usage`, 13 lines) and `tapdisk_syslog_facility_by_name`
  (`tapdisk-utils.c:77-101`): run the instrumented `tapdisk -h` and a facility argument if there is one.
- [-] `lowmem_*`, `reset_timeout`, `cpumond_*`, `tapdisk_server_initialize`: memory pressure, CPU monitor and the
  stream tool; not worth a test.

---

# Part 2: the rest

## 2.1 Command-line tools of the tree

- [x] `vhd-util` (`test_blktap_cov_vhd_util.py`): create, query, fill, read, snapshot, scan, coalesce, copy, resize,
  modify/set, key, invalid files. `libvhd.c` 80 % of functions, `libvhd-journal.c` 88 %.
- [ ] Raise the weak ones, with the arguments of the usage of each command: `vhd-util-fill.c` (28 %),
  `-modify.c` (37 %), `-copy.c` (41 %), `-resize.c` (42 %: shrink, fixed disks, `-j` journal), `-scan.c` (51 %:
  `-p -m -v -a`, a directory, a broken parent), `-check.c` (57 %: corrupted file variants), `-revert.c`.
- [ ] `vhd-index` (`libvhd-index.c` 0 %, `vhd-index.c`): only if 1.9 keeps the index stack.
- [ ] `cbt-util` (use `blktap_tool`, see 1.2b), `td-util` (`drivers/td.c`, 10 functions, `tapdisk_get_image_size`),
  `lvm-util` (`lvm/lvm-util.c` 3 %: read-only commands on the existing VG).

## 2.2 `tap-ctl` command line

- [x] `test_blktap_cov_tapctl_cli.py`: help of every command, failing commands, `major`, `check`, `list` filters,
  `destroy`. `tap-ctl.c` 98 % of functions.
- [ ] Remaining: `tap_ctl_find_minor`, `tap_cli_list_row` output formats, `stats` of all VBDs (no `-m`:
  `tapdisk-control.c:1205`, 5 lines), pending connections (`tapdisk-control.c:1660`).

## 2.3 tapback

- [ ] `tapback/` is at 71-89 % of functions: `tapback_backend_scan`, `tapback_write_pid`, `usage`, `compare`,
  `tapback_signal_handler` (master or `_exit()` paths: no counters), `find_tapdisk_by_pid`,
  `physical_device_path_changed`, `tapback_find_slave`. Ask the user whether a coverage-only `__gcov_dump()`
  before `_exit()` (under an `#ifdef`) is acceptable; without it the master's counters are lost.
- [ ] Change the `params` of a connected VBD in XenStore while the VM runs (what XAPI does on a path change):
  `physical_device_path_changed`.

---

# Report to produce at the end

For each ticked box: test added (`file::name`), host and VM used, the functions and lines now hit (before / after,
from the lcov output), what could not be done and why (with the reading of the code that supports it), and the
candidates for `cov-filters.conf` or removal. Do not claim a function is covered without having looked at the
lcov output.
