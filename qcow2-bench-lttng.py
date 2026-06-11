#!/usr/bin/env python3
"""
Convert a babeltrace2 text trace of qcow2-bench LTTng events to a Perfetto
JSON trace (Trace Event Format) for perfetto.dev/ui.

Usage:
    python3 qcow2-bench-lttng.py lttng-trace.log > trace.json
    # then open trace.json in https://ui.perfetto.dev

Events handled:
    qcow2:read_enter / read_return     -> slice on "CPU N / IO" track
    qcow2:write_enter / write_return   -> slice on "CPU N / IO" track
    qcow2:lock_wait / lock_acquired    -> slice on "CPU N / Lock wait" track
    qcow2:lock_acquired / lock_release -> slice on "CPU N / Lock held" track
    qcow2:complete_enter / complete_return -> "blk_aio cb (iothread)" track --
                                          the blk_aio callback (trampoline), which
                                          ALWAYS runs on the IOThread; it just
                                          pushes to the ring (not the real work).
    qcow2:complete_enter / tapdisk:complete_vbd_enter /
    tapdisk:complete_vbd_return / qcow2:complete_return ->
                                          "completion phases" track -- splits the
                                          inline completion into qcow2-code vs
                                          tapdisk-code phases (see below). Only
                                          meaningful when completion runs inline
                                          on the IOThread (no completion thread):
                                          the four marks then land on the same CPU
                                          in order and are matched per CPU.
    tapdisk:driver_queue / driver_complete -> "Driver RTT / queue N" track
                                          (full qcow2-driver RTT, keyed by req_id)
    complete_return -> driver_complete -> "completion (deferred)" slice on the
                                          completion thread's *real* CPU (matched
                                          by offset): ring wait + actual
                                          td_complete_request/grant-copy work.
    qcow2:comutex_wait / comutex_acquired -> slice on "CoMutex contention" track
    qcow2:comutex_release              -> instant event on the holder's CPU track

Track layout (one Perfetto "process" per cpu_id, grouped in the UI):
    CPU N
      IO          -- read/write request spans
      Lock wait   -- time blocked waiting for s->lock
      Lock held   -- time holding s->lock (L2 lookup / alloc)
      Completion  -- time spent in the request completion callback
      completion phases -- the inline completion split into adjacent slices:
                    "qcow2: acct" (complete_enter -> complete_vbd_enter, the
                    s->lock + aio_inflight bookkeeping), "tapdisk:
                    td_complete_request" (complete_vbd_enter ->
                    complete_vbd_return, the vbd cb chain / grant copy),
                    the kick (complete_vbd_return -> complete_kicked,
                    tapdisk_vbd_kick) and "qcow2: free" (complete_kicked ->
                    complete_return, free_qcow2_request). When the kick_*
                    tracepoints are present the kick is further split into
                    "kick: mutex wait" / "kick: drain" / "kick: wake"; otherwise
                    it shows as a single "tapdisk: kick". Older traces without
                    complete_kicked fall back to a single "qcow2: kick+free".
    CoMutex contention   -- rare generic CoMutex blocks (all mutexes, not just s->lock)
"""

import re
import sys
import json
from array import array

# ---------------------------------------------------------------------------
# Regex
# ---------------------------------------------------------------------------
RE_LINE = re.compile(
    r'\[(\d{2}:\d{2}:\d{2})\.(\d{9})\]'   # [HH:MM:SS.nnnnnnnnn]
    r'\s+\(\+[^\)]+\)'                      # (+delta)
    r'\s+\S+'                               # hostname
    r'\s+((?:qcow2|tapdisk):\w+):'         # event name (qcow2 or tapdisk provider)
    r'\s+\{[^}]+\}'                         # context block  { cpu_id = N }
    r',\s+\{([^}]+)\}'                      # payload block  { k = v, ... }
)
RE_CPU = re.compile(r'cpu_id\s*=\s*(\d+)')
RE_KV  = re.compile(r'(\w+)\s*=\s*(0x[0-9A-Fa-f]+|-?\d+)')

# Within each CPU "process", virtual threads:
TID_IO       = 0
TID_LOCKWAIT = 1
TID_LOCKHELD = 2
TID_COMPLETE = 3   # blk_aio cb (trampoline) -- runs on the IOThread
TID_DEFCOMP  = 4   # deferred completion -- runs on the completion thread's CPU
TID_SUBMIT   = 5   # submission (driver_queue -> read_enter) -- tapdisk core thread
TID_CPHASE   = 6   # completion phases (qcow2 vs tapdisk split), per CPU
PID_COMUTEX  = 999
PID_RTT      = 1000   # tapdisk driver RTT (driver_queue -> driver_complete), tid = queue


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def ts_ns(hms: str, frac: str) -> int:
    h, m, s = hms.split(':')
    return (int(h) * 3600 + int(m) * 60 + int(s)) * 10**9 + int(frac)


def parse_fields(payload: str) -> dict:
    return {k: int(v, 0) for k, v in RE_KV.findall(payload)}


def us(ns: int) -> float:
    return ns / 1000.0


def short_addr(addr: int) -> str:
    """Last 16 bits of address as 0xXXXX for readable labels."""
    return f"0x{addr & 0xFFFF:04X}"


# ---------------------------------------------------------------------------
# Converter
# ---------------------------------------------------------------------------

def convert(path: str, out) -> dict:
    # The trace is huge (hundreds of MB of events), so we never materialise the
    # full event list: each event is serialised straight to `out` as it is
    # produced (Trace Event Format does not require ordering, Perfetto sorts by
    # ts). For the stderr summary we keep only the per-category durations in
    # compact arrays (8 bytes/value) instead of the event dicts.
    buckets: dict[str, array] = {}
    wrote_any = False
    meta_seen: set = set()

    # open slots keyed by coroutine address
    io_open:       dict[int, tuple[int, dict]] = {}
    lockwait_open: dict[int, tuple[int, dict]] = {}
    lockheld_open: dict[int, tuple[int, dict]] = {}
    cmwait_open:   dict[int, tuple[int, dict]] = {}
    complete_open: dict[int, tuple[int, dict]] = {}  # req -> (ts, fields)
    # per-CPU in-progress completion, for the qcow2/tapdisk phase split. Inline
    # completion runs to completion on one thread, so a single open slot per CPU
    # is enough (no nesting). key: cpu -> dict(t_enter, req, offset, t_vbd_enter,
    # t_vbd_return, queue, req_id)
    cphase_open:   dict[int, dict] = {}
    rtt_open:      dict[tuple, tuple[int, dict]] = {}  # (queue, req_id) -> (ts, fields)
    cret_open:     dict[int, list] = {}  # offset -> [ts,...]  (trampoline end -> driver_complete)
    submit_open:   dict[int, list] = {}  # offset -> [(ts, cpu),...]  (driver_queue -> read_enter)

    def write_obj(e):
        nonlocal wrote_any
        if wrote_any:
            out.write(',')
        wrote_any = True
        out.write(json.dumps(e, separators=(',', ':')))

    def emit(name, ph, pid, tid, t, dur=None, args=None, cat=None):
        e = {"name": name, "ph": ph, "pid": pid, "tid": tid, "ts": us(t)}
        if dur is not None:
            e["dur"] = us(dur)
        if args:
            e["args"] = args
        if cat:
            e["cat"] = cat
        write_obj(e)
        if ph == 'X' and dur is not None and cat:
            b = buckets.get(cat)
            if b is None:
                b = buckets[cat] = array('d')
            b.append(us(dur))

    def meta(pid, tid, pname, tname):
        key = (pid, tid)
        if key not in meta_seen:
            meta_seen.add(key)
            write_obj({"ph": "M", "pid": pid, "tid": tid,
                       "name": "process_name", "args": {"name": pname}})
            write_obj({"ph": "M", "pid": pid, "tid": tid,
                       "name": "thread_name",  "args": {"name": tname}})

    base_ns = None

    with open(path) as f:
        for line in f:
            m = RE_LINE.match(line)
            if not m:
                continue
            hms, frac, evname, payload = m.groups()
            cpu_m = RE_CPU.search(line)
            cpu = int(cpu_m.group(1)) if cpu_m else 0
            fields = parse_fields(payload)
            t = ts_ns(hms, frac)
            if base_ns is None:
                base_ns = t
            t -= base_ns
            pid = cpu  # one Perfetto "process" per cpu_id

            # ---- I/O spans: read_enter/return, write_enter/return ----------
            if evname in ('qcow2:read_enter', 'qcow2:write_enter'):
                co  = fields['co']
                op  = 'read' if evname.endswith('read_enter') else 'write'
                meta(pid, TID_IO, f"CPU {cpu}", "IO")
                io_open[co] = (t, {'offset': fields['offset'],
                                   'bytes':  fields['bytes'],
                                   'op':     op})
                # close the submission span (driver_queue -> read_enter) on the
                # submitting (tapdisk core) thread's real CPU, matched by offset
                sq = submit_open.get(fields['offset'])
                if sq:
                    t0, scpu = sq.pop(0)
                    meta(scpu, TID_SUBMIT, f"CPU {scpu}", "submission")
                    emit("submission", 'X', scpu, TID_SUBMIT, t0, dur=t - t0,
                         args={'offset': fields['offset'],
                               'submit_us': round(us(t - t0), 3)},
                         cat='submit')

            elif evname in ('qcow2:read_return', 'qcow2:write_return'):
                co = fields['co']
                if co in io_open:
                    t0, info = io_open.pop(co)
                    off_mib = info['offset'] >> 20
                    label = f"{info['op']} @{off_mib}MiB"
                    emit(label, 'X', pid, TID_IO, t0, dur=t - t0,
                         args={**info, 'ret': fields.get('ret', 0)},
                         cat=info['op'])

            # ---- Lock wait: lock_wait -> lock_acquired ---------------------
            elif evname == 'qcow2:lock_wait':
                co = fields['co']
                meta(pid, TID_LOCKWAIT, f"CPU {cpu}", "Lock wait")
                lockwait_open[co] = (t, {'offset': fields['offset']})

            elif evname == 'qcow2:lock_acquired':
                co = fields['co']
                if co in lockwait_open:
                    t0, info = lockwait_open.pop(co)
                    dur = t - t0
                    emit("lock wait", 'X', pid, TID_LOCKWAIT, t0, dur=dur,
                         args={**info, 'wait_us': round(us(dur), 3)},
                         cat='lock_wait')
                # open held slot
                meta(pid, TID_LOCKHELD, f"CPU {cpu}", "Lock held")
                lockheld_open[co] = (t, {'offset': fields['offset']})

            elif evname == 'qcow2:lock_release':
                co = fields['co']
                if co in lockheld_open:
                    t0, info = lockheld_open.pop(co)
                    dur = t - t0
                    emit("lock held", 'X', pid, TID_LOCKHELD, t0, dur=dur,
                         args={**info, 'held_us': round(us(dur), 3)},
                         cat='lock_held')

            # ---- blk_aio callback / trampoline (runs on the IOThread) --------
            # complete_enter/return wrap acb->common.cb() in blk_aio_complete,
            # which always runs on the BB's AioContext = the IOThread. With the
            # offload this is just "push to ring", so it stays on the IOThread's
            # CPU -- it is NOT the actual completion work.
            elif evname == 'qcow2:complete_enter':
                req = fields['req']
                meta(pid, TID_COMPLETE, f"CPU {cpu}", "blk_aio cb (iothread)")
                complete_open[req] = (t, {'offset': fields['offset'],
                                          'ret':    fields.get('ret', 0)})
                # start the per-CPU phase record (inline completion only)
                cphase_open[cpu] = {'t_enter': t, 'req': req,
                                    'offset': fields['offset'],
                                    't_vbd_enter': None, 't_vbd_return': None,
                                    't_kicked': None, 'inflight': None,
                                    't_kick_locked': None, 't_kick_drained': None,
                                    't_kick_cb': None, 'kick_loops': 0,
                                    'drained': None, 'woke': None,
                                    'queue': None, 'req_id': None}

            # ---- completion phase boundaries (inline completion, same CPU) ----
            # td_complete_request is bracketed by the tapdisk provider; pin them
            # onto the CPU's currently-open completion so we can split it.
            elif evname == 'tapdisk:complete_vbd_enter':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    rec['t_vbd_enter'] = t
                    rec['queue'] = fields.get('queue')
                    rec['req_id'] = fields.get('req_id')

            elif evname == 'tapdisk:complete_vbd_return':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    rec['t_vbd_return'] = t
                    rec['inflight'] = fields.get('inflight')

            elif evname == 'tapdisk:complete_kicked':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    rec['t_kicked'] = t

            # ---- tapdisk_vbd_kick() internal phases (same CPU, inline) -------
            elif evname == 'tapdisk:kick_locked':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    rec['t_kick_locked'] = t

            elif evname == 'tapdisk:kick_loop':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    rec['kick_loops'] += 1

            elif evname == 'tapdisk:kick_final_cb':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    # keep the last one: the final cb of the last token group
                    rec['t_kick_cb'] = t

            elif evname == 'tapdisk:kick_drained':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    rec['t_kick_drained'] = t
                    rec['drained'] = fields.get('drained')

            elif evname == 'tapdisk:kick_return':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    rec['woke'] = fields.get('woke')

            elif evname == 'qcow2:complete_return':
                req = fields['req']
                if req in complete_open:
                    t0, info = complete_open.pop(req)
                    dur = t - t0
                    emit("blk_aio cb", 'X', pid, TID_COMPLETE, t0, dur=dur,
                         args={**info, 'cb_us': round(us(dur), 3)},
                         cat='trampoline')
                    # hand off to the deferred-completion matcher (by offset)
                    cret_open.setdefault(info['offset'], []).append(t)

                # emit the qcow2/tapdisk phase split on the per-CPU track
                rec = cphase_open.pop(cpu, None)
                if rec is not None and rec['req'] == req:
                    meta(pid, TID_CPHASE, f"CPU {cpu}", "completion phases")
                    base = {'queue': rec['queue'], 'req_id': rec['req_id'],
                            'offset': rec['offset'], 'inflight': rec['inflight']}
                    ve, vr = rec['t_vbd_enter'], rec['t_vbd_return']
                    kk = rec['t_kicked']
                    if ve is not None and vr is not None:
                        emit("qcow2: acct", 'X', pid, TID_CPHASE,
                             rec['t_enter'], dur=ve - rec['t_enter'],
                             args={**base, 'us': round(us(ve - rec['t_enter']), 3)},
                             cat='cphase_qcow2_pre')
                        emit("tapdisk: td_complete_request", 'X', pid, TID_CPHASE,
                             ve, dur=vr - ve,
                             args={**base, 'us': round(us(vr - ve), 3)},
                             cat='cphase_tapdisk')
                        if kk is not None:
                            kl, kd = rec['t_kick_locked'], rec['t_kick_drained']
                            if kl is not None and kd is not None:
                                # split the kick itself: mutex wait / drain /
                                # wake (anchored on vr and kk to stay contiguous)
                                emit("kick: mutex wait", 'X', pid, TID_CPHASE,
                                     vr, dur=kl - vr,
                                     args={**base, 'us': round(us(kl - vr), 3)},
                                     cat='cphase_kick_wait')
                                kcb = rec['t_kick_cb']
                                dargs = {**base, 'drained': rec['drained'],
                                         'loops': rec['kick_loops']}
                                if kcb is not None and kl <= kcb <= kd:
                                    # isolate the final cb (last response push)
                                    emit("kick: drain", 'X', pid, TID_CPHASE,
                                         kl, dur=kcb - kl,
                                         args={**dargs,
                                               'us': round(us(kcb - kl), 3)},
                                         cat='cphase_kick_drain')
                                    emit("kick: final cb", 'X', pid, TID_CPHASE,
                                         kcb, dur=kd - kcb,
                                         args={**dargs,
                                               'us': round(us(kd - kcb), 3)},
                                         cat='cphase_kick_finalcb')
                                else:
                                    emit("kick: drain", 'X', pid, TID_CPHASE,
                                         kl, dur=kd - kl,
                                         args={**dargs,
                                               'us': round(us(kd - kl), 3)},
                                         cat='cphase_kick_drain')
                                emit("kick: wake", 'X', pid, TID_CPHASE,
                                     kd, dur=kk - kd,
                                     args={**base, 'woke': rec['woke'],
                                           'us': round(us(kk - kd), 3)},
                                     cat='cphase_kick_wake')
                            else:
                                # no kick sub-marks (notify was false: no kick)
                                emit("tapdisk: kick", 'X', pid, TID_CPHASE,
                                     vr, dur=kk - vr,
                                     args={**base, 'us': round(us(kk - vr), 3)},
                                     cat='cphase_kick')
                            emit("qcow2: free", 'X', pid, TID_CPHASE,
                                 kk, dur=t - kk,
                                 args={**base, 'us': round(us(t - kk), 3)},
                                 cat='cphase_free')
                        else:
                            emit("qcow2: kick+free", 'X', pid, TID_CPHASE,
                                 vr, dur=t - vr,
                                 args={**base, 'us': round(us(t - vr), 3)},
                                 cat='cphase_qcow2_post')
                    else:
                        # no vbd marks: multi-part AIO that returned early
                        # (aio_inflight still > 0), so signal_completion bailed
                        # before td_complete_request.
                        emit("qcow2: acct (partial)", 'X', pid, TID_CPHASE,
                             rec['t_enter'], dur=t - rec['t_enter'],
                             args={**base, 'us': round(us(t - rec['t_enter']), 3)},
                             cat='cphase_partial')

            # ---- Generic CoMutex contention (all CoMutexes, not just s->lock)
            elif evname == 'qcow2:comutex_wait':
                self_ = fields['self']
                same  = fields['self_ctx'] == fields['holder_ctx']
                meta(PID_COMUTEX, 0, "CoMutex contention", "contention")
                cmwait_open[self_] = (t, {
                    'mutex':      hex(fields['mutex']),
                    'holder':     hex(fields['holder']),
                    'holder_ctx': hex(fields['holder_ctx']),
                    'self_ctx':   hex(fields['self_ctx']),
                    'same_ctx':   same,
                })

            elif evname == 'qcow2:comutex_acquired':
                self_ = fields['self']
                if self_ in cmwait_open:
                    t0, info = cmwait_open.pop(self_)
                    dur   = t - t0
                    ctx   = 'same' if info['same_ctx'] else 'cross'
                    label = (f"wait {short_addr(self_)} on "
                             f"{short_addr(fields['mutex'])} ({ctx}-ctx)")
                    emit(label, 'X', PID_COMUTEX, 0, t0, dur=dur,
                         args={**info, 'blocked_us': round(us(dur), 3)},
                         cat='comutex')

            elif evname == 'qcow2:comutex_release':
                # instant mark on the releasing CPU's IO track
                self_ = fields['self']
                meta(pid, TID_IO, f"CPU {cpu}", "IO")
                emit(f"comutex release {short_addr(self_)}", 'i',
                     pid, TID_IO, t,
                     args={'mutex': hex(fields['mutex'])},
                     cat='comutex')

            # ---- Driver RTT: tapdisk:driver_queue -> driver_complete --------
            # The whole qcow2-driver round trip as seen by tapdisk, keyed by
            # (queue, req_id). This is the outer span enclosing the qcow2:*
            # events; shown on a per-queue "Driver RTT" track.
            elif evname == 'tapdisk:driver_queue':
                q = fields['queue']
                key = (q, fields['req_id'])
                meta(PID_RTT, q, "Driver RTT", f"queue {q}")
                rtt_open[key] = (t, {'req_id': fields['req_id'],
                                     'op':     fields['op'],
                                     'sector': fields['sector'],
                                     'secs':   fields.get('secs', 0)})
                # remember where/when the request was queued (submitting CPU),
                # to draw the submission span when read_enter picks it up
                submit_open.setdefault(fields['sector'] * 512, []).append((t, cpu))

            elif evname == 'tapdisk:driver_complete':
                q = fields['queue']
                key = (q, fields['req_id'])
                if key in rtt_open:
                    t0, info = rtt_open.pop(key)
                    dur = t - t0
                    op = 'read' if info['op'] == 0 else 'write'
                    label = f"req {info['req_id']} {op}"
                    emit(label, 'X', PID_RTT, q, t0, dur=dur,
                         args={**info, 'rtt_us': round(us(dur), 3)},
                         cat='rtt')
                # Deferred completion: from the blk_aio trampoline handoff
                # (complete_return) to here (td_complete_request on the
                # completion thread). Shown on driver_complete's *real* CPU =
                # the completion thread's CPU, so you can see it run in
                # parallel with (or not) the IOThread. Matched by offset.
                off = fields['sector'] * 512
                lst = cret_open.get(off)
                if lst:
                    t0 = lst.pop(0)
                    meta(cpu, TID_DEFCOMP, f"CPU {cpu}", "completion (deferred)")
                    emit("deferred completion", 'X', cpu, TID_DEFCOMP, t0,
                         dur=t - t0,
                         args={'queue': q, 'req_id': fields['req_id'],
                               'defer_us': round(us(t - t0), 3)},
                         cat='defcomp')

    return buckets


# ---------------------------------------------------------------------------
# Stats summary to stderr
# ---------------------------------------------------------------------------

def summarise(buckets: dict):
    import statistics as st

    def row(label, xs):
        if not xs:
            return
        xs = sorted(xs)
        p = lambda q: xs[min(len(xs) - 1, int(q * len(xs)))]
        print(f"  {label:18}: n={len(xs):7}  "
              f"mean={st.mean(xs):8.2f}µs  "
              f"p50={p(.50):8.2f}  p99={p(.99):8.2f}  max={xs[-1]:8.2f}µs",
              file=sys.stderr)

    print("=== summary ===", file=sys.stderr)
    row("driver RTT",      buckets.get('rtt', []))
    row("submission",      buckets.get('submit', []))
    row("io span (read)",  buckets.get('read', []))
    row("io span (write)", buckets.get('write', []))
    row("lock wait",       buckets.get('lock_wait', []))
    row("lock held",       buckets.get('lock_held', []))
    row("blk_aio cb (iothread)", buckets.get('trampoline', []))
    row("  phase qcow2 acct",    buckets.get('cphase_qcow2_pre', []))
    row("  phase tapdisk",       buckets.get('cphase_tapdisk', []))
    row("  phase kick mutex wait", buckets.get('cphase_kick_wait', []))
    row("  phase kick drain",    buckets.get('cphase_kick_drain', []))
    row("  phase kick final cb", buckets.get('cphase_kick_finalcb', []))
    row("  phase kick wake",     buckets.get('cphase_kick_wake', []))
    row("  phase tapdisk kick",  buckets.get('cphase_kick', []))
    row("  phase qcow2 free",    buckets.get('cphase_free', []))
    row("  phase qcow2 kick+free", buckets.get('cphase_qcow2_post', []))
    row("  phase partial",       buckets.get('cphase_partial', []))
    row("deferred completion",   buckets.get('defcomp', []))
    row("comutex wait",    buckets.get('comutex', []))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} lttng-trace.log > trace.json",
              file=sys.stderr)
        sys.exit(1)

    out = sys.stdout
    out.write('{"traceEvents":[')
    buckets = convert(sys.argv[1], out)
    out.write('],"displayTimeUnit":"ms"}\n')
    summarise(buckets)
