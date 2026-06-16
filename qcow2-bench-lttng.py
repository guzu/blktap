#!/usr/bin/env python3
"""
Convert a babeltrace2 text trace of qcow2-bench LTTng events to a Perfetto
JSON trace (Trace Event Format) for perfetto.dev/ui.

Usage:
    python3 qcow2-bench-lttng.py lttng-trace.log > trace.json
    # cap the output size (k/m/g, optional 'b', case-insensitive):
    python3 qcow2-bench-lttng.py -m 50MB lttng-trace.log > trace.json
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
    tapdisk:sched_enter / sched_wait / sched_dispatch / sched_return ->
                                          "Scheduler" track of the CPU that ran the
                                          iteration. One scheduler_wait_for_events()
                                          iteration split into prepare / select
                                          (blocked) / check / dispatch / gc+unlock
                                          phases; each phase is placed on the CPU
                                          recorded at its starting mark (the thread
                                          may migrate across select()). The
                                          scheduler id (0xFFFF = global control
                                          scheduler) is carried in args['sched'].
    tapdisk:sched_event                -> "event cb" slice nested in the dispatch
                                          phase (one per event->cb()); labelled
                                          with the fd and the callback address.
    tapdisk:ring_lock / ring_dispatch  -> "ring" track of the CPU that ran
                                          tapdisk_xenio_ctx_process_ring(): "ring:
                                          mutex wait" (ring_lock begin -> end) and
                                          "ring: drain (N)" (ring_lock end ->
                                          ring_dispatch, N requests copied out and
                                          issued to the driver chain).

    Bracketing tracepoints carry their begin/end in a "phase" field encoded with
    the ASCII markers TP_PHASE_BEGIN ("BEGN") / TP_PHASE_END ("END!"); the legacy
    0/1 encoding is still accepted (see is_begin()).

Track layout (one Perfetto "process" per cpu_id, grouped in the UI):
    CPU N
      IO          -- read/write request spans
      Lock wait   -- time blocked waiting for s->lock
      Lock held   -- time holding s->lock (L2 lookup / alloc)
      Completion  -- time spent in the request completion callback
      Scheduler   -- scheduler_wait_for_events() phases (prepare / select /
                    check / dispatch / gc+unlock), with each event->cb() nested
                    in the dispatch phase
      ring        -- tapdisk_xenio_ctx_process_ring() phases: mutex wait then
                    drain (descriptors copied out + issued to the driver)
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
                    it shows as a single "tapdisk: kick". When present, the grant
                    copy ("guest copy", guest_copy2 -- usually the bulk of the
                    final cb) and the guest-notify hypercall ("evtchn notify",
                    sparse/batched) are shown as slices nested inside the kick.
                    Older traces without complete_kicked fall back to a single
                    "qcow2: kick+free".
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
TID_SCHED    = 7   # scheduler phases, per CPU (the CPU that ran the iteration)
TID_RING     = 8   # process_ring phases (mutex wait / drain), per CPU
PID_COMUTEX  = 999
PID_RTT      = 1000   # tapdisk driver RTT (driver_queue -> driver_complete), tid = queue

# Phase markers carried by the bracketing tracepoints (begin/end). They are
# ASCII magic values so a raw trace reads unambiguously; older traces used a
# bare 0/1, which is_begin() still accepts.
TP_PHASE_BEGIN = 0x4245474e  # "BEGN"
TP_PHASE_END   = 0x454e4421  # "END!"


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


def is_begin(phase) -> bool:
    """True for the begin marker of a bracketing tracepoint.

    Accepts the ASCII marker (TP_PHASE_BEGIN) and the legacy 0 encoding;
    anything else (TP_PHASE_END or the legacy 1) is an end marker."""
    return phase == TP_PHASE_BEGIN or phase == 0


def sched_tname(sid: int) -> str:
    """Thread name for a scheduler id (0xFFFF is the global control scheduler)."""
    return "global" if sid == 0xFFFF else f"queue {sid}"


_SIZE_MULT = {'': 1, 'k': 1024, 'm': 1024**2, 'g': 1024**3}


def parse_size(s: str) -> int:
    """Parse a human-readable byte size into an int.

    Accepts a plain byte count or a k/m/g suffix, case-insensitive, with an
    optional trailing 'b': e.g. 500000, 512k, 512KB, 10m, 10MB, 1G, 1gb."""
    m = re.fullmatch(r'\s*(\d+(?:\.\d+)?)\s*([kKmMgG]?)[bB]?\s*', s)
    if not m:
        raise ValueError(f"invalid size: {s!r}")
    return int(float(m.group(1)) * _SIZE_MULT[m.group(2).lower()])


# ---------------------------------------------------------------------------
# Converter
# ---------------------------------------------------------------------------

def convert(path: str, out, max_bytes=None) -> dict:
    # The trace is huge (hundreds of MB of events), so we never materialise the
    # full event list: each event is serialised straight to `out` as it is
    # produced (Trace Event Format does not require ordering, Perfetto sorts by
    # ts). For the stderr summary we keep only the per-category durations in
    # compact arrays (8 bytes/value) instead of the event dicts.
    #
    # max_bytes, when set, is the byte budget for the emitted events (the caller
    # has already reserved the JSON envelope): once writing the next event would
    # exceed it we stop emitting and break out of the parse loop, leaving a
    # valid (truncated) trace.
    buckets: dict[str, array] = {}
    wrote_any = False
    truncated = False
    nbytes = 0
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
    sched_open:    dict[int, list] = {}  # sched id -> stack of in-progress iterations
    schedev_open:  dict[int, tuple] = {} # sched id -> (ts, fields) of the current event cb
    ring_open:     dict[int, dict] = {}  # queue -> in-progress process_ring() marks

    def write_obj(e):
        nonlocal wrote_any, nbytes, truncated
        if truncated:
            return
        s = json.dumps(e, separators=(',', ':'))
        add = len(s) + (1 if wrote_any else 0)   # +1 for the separating comma
        if max_bytes is not None and nbytes + add > max_bytes:
            truncated = True
            return
        if wrote_any:
            out.write(',')
        wrote_any = True
        out.write(s)
        nbytes += add

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
            if truncated:
                break
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
                                    't_evtchn0': None, 't_evtchn1': None,
                                    't_gc0': None, 't_gc1': None,
                                    'gc_dir': None, 'gc_segs': None,
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

            # ---- grant copy to/from the guest (guest_copy2), inside the kick.
            # phase = TP_PHASE_BEGIN/END; direction 0 = from guest (write),
            # 1 = to guest (read completion). This is usually the bulk of the
            # final cb.
            elif evname == 'tapdisk:guest_copy':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    if is_begin(fields.get('phase')):
                        rec['t_gc0'] = t
                        rec['gc_dir'] = fields.get('direction')
                        rec['gc_segs'] = fields.get('nr_segments')
                    else:
                        rec['t_gc1'] = t

            # ---- guest notification (xenevtchn_notify), inside the kick drain
            # phase = TP_PHASE_BEGIN (before the hypercall) / END. Sparse (batched).
            elif evname == 'tapdisk:evtchn_notify':
                rec = cphase_open.get(cpu)
                if rec is not None:
                    if is_begin(fields.get('phase')):
                        rec['t_evtchn0'] = t
                    else:
                        rec['t_evtchn1'] = t

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
                            # grant copy (guest_copy2), nested inside the kick:
                            # usually the bulk of the final cb
                            g0, g1 = rec['t_gc0'], rec['t_gc1']
                            if g0 is not None and g1 is not None and g0 <= g1:
                                d = 'rd' if rec['gc_dir'] == 1 else 'wr'
                                emit(f"guest copy ({d})", 'X', pid, TID_CPHASE,
                                     g0, dur=g1 - g0,
                                     args={**base, 'direction': rec['gc_dir'],
                                           'nr_segments': rec['gc_segs'],
                                           'us': round(us(g1 - g0), 3)},
                                     cat='cphase_guest_copy')
                            # guest notify hypercall, nested inside the kick
                            # (only when it actually fired -- batched, sparse)
                            e0, e1 = rec['t_evtchn0'], rec['t_evtchn1']
                            if e0 is not None and e1 is not None and e0 <= e1:
                                emit("evtchn notify", 'X', pid, TID_CPHASE,
                                     e0, dur=e1 - e0,
                                     args={**base, 'us': round(us(e1 - e0), 3)},
                                     cat='cphase_evtchn')
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

            # ---- Scheduler phases: sched_enter/wait/dispatch/return ----------
            # One scheduler_wait_for_events() iteration, shown on a "Scheduler"
            # track of the CPU that ran it. The thread can migrate across the
            # blocking select(), so each phase is attributed to the CPU recorded
            # at its starting mark (cpu_*). Recursive invocations (depth > 1)
            # nest via a per-id stack; the depth>1 early-out path has no
            # sched_wait (no select).
            elif evname == 'tapdisk:sched_enter':
                sched_open.setdefault(fields['id'], []).append({
                    't_enter': t, 'cpu_enter': cpu, 'depth': fields.get('depth'),
                    't_wait0': None, 'cpu_wait0': None,
                    't_wait1': None, 'cpu_wait1': None,
                    'timeout': None, 'nfds': None,
                    't_disp0': None, 'cpu_disp0': None,
                    't_disp1': None, 'cpu_disp1': None, 'dispatched': None})

            elif evname == 'tapdisk:sched_wait':
                st = sched_open.get(fields['id'])
                if st:
                    rec = st[-1]
                    if is_begin(fields.get('phase')):
                        rec['t_wait0'] = t
                        rec['cpu_wait0'] = cpu
                        rec['timeout'] = fields.get('val')
                    else:
                        rec['t_wait1'] = t
                        rec['cpu_wait1'] = cpu
                        rec['nfds'] = fields.get('val')

            elif evname == 'tapdisk:sched_dispatch':
                st = sched_open.get(fields['id'])
                if st:
                    rec = st[-1]
                    if is_begin(fields.get('phase')):
                        rec['t_disp0'] = t
                        rec['cpu_disp0'] = cpu
                    else:
                        rec['t_disp1'] = t
                        rec['cpu_disp1'] = cpu
                        rec['dispatched'] = fields.get('n')

            # one event->cb() inside the dispatch phase; nests on the same track
            elif evname == 'tapdisk:sched_event':
                sid = fields['id']
                if is_begin(fields.get('phase')):
                    schedev_open[sid] = (t, cpu, {'fd': fields.get('fd'),
                                                  'mode': fields.get('mode'),
                                                  'cb': hex(fields['cb'])
                                                  if 'cb' in fields else None})
                else:
                    so = schedev_open.pop(sid, None)
                    if so is not None:
                        t0, ecpu, info = so
                        meta(ecpu, TID_SCHED, f"CPU {ecpu}", "Scheduler")
                        label = f"event cb fd={info['fd']}"
                        if info.get('cb'):
                            label += f" {short_addr(int(info['cb'], 16))}"
                        emit(label, 'X', ecpu, TID_SCHED, t0, dur=t - t0,
                             args={**info, 'id': sid, 'sched': sched_tname(sid),
                                   'us': round(us(t - t0), 3)},
                             cat='sched_event')

            elif evname == 'tapdisk:sched_return':
                st = sched_open.get(fields['id'])
                if st:
                    rec = st.pop()
                    sid = fields['id']
                    base = {'id': sid, 'sched': sched_tname(sid),
                            'depth': rec['depth']}
                    # ordered phase boundaries; emit a span between consecutive
                    # marks on the CPU recorded at the span's start mark, the
                    # last one running to sched_return (t).
                    segs = [(rec['t_enter'], rec['cpu_enter'],
                             "prepare", 'sched_prepare', {})]
                    if rec['t_wait0'] is not None:
                        segs.append((rec['t_wait0'], rec['cpu_wait0'],
                                     "select (blocked)", 'sched_select',
                                     {'timeout_ms': rec['timeout'],
                                      'nfds': rec['nfds']}))
                    if rec['t_wait1'] is not None:
                        segs.append((rec['t_wait1'], rec['cpu_wait1'],
                                     "check", 'sched_check', {}))
                    if rec['t_disp0'] is not None:
                        segs.append((rec['t_disp0'], rec['cpu_disp0'],
                                     "dispatch", 'sched_dispatch',
                                     {'dispatched': rec['dispatched']}))
                    if rec['t_disp1'] is not None:
                        segs.append((rec['t_disp1'], rec['cpu_disp1'],
                                     "gc + unlock", 'sched_post', {}))
                    for i, (ts0, c0, label, cat, extra) in enumerate(segs):
                        ts1 = segs[i + 1][0] if i + 1 < len(segs) else t
                        if ts1 < ts0:
                            continue
                        meta(c0, TID_SCHED, f"CPU {c0}", "Scheduler")
                        emit(label, 'X', c0, TID_SCHED, ts0, dur=ts1 - ts0,
                             args={**base, **extra,
                                   'us': round(us(ts1 - ts0), 3)},
                             cat=cat)

            # ---- process_ring phases: ring_lock (mutex wait) / ring_dispatch -
            # tapdisk_xenio_ctx_process_ring() drains the shared ring under
            # queue->mutex then dispatches. Keyed by queue (the thread can
            # migrate across the mutex wait), each phase on its start CPU:
            #   "ring: mutex wait" (ring_lock begin -> end)
            #   "ring: drain (N)"  (ring_lock end -> ring_dispatch, N requests
            #                       copied out then issued to the driver chain)
            elif evname == 'tapdisk:ring_lock':
                q = fields['queue']
                if is_begin(fields.get('phase')):
                    ring_open[q] = {'t0': t, 'cpu0': cpu,
                                    't1': None, 'cpu1': None}
                else:
                    rec = ring_open.get(q)
                    if rec is not None:
                        rec['t1'] = t
                        rec['cpu1'] = cpu
                        meta(rec['cpu0'], TID_RING, f"CPU {rec['cpu0']}", "ring")
                        emit("ring: mutex wait", 'X', rec['cpu0'], TID_RING,
                             rec['t0'], dur=t - rec['t0'],
                             args={'queue': q,
                                   'us': round(us(t - rec['t0']), 3)},
                             cat='ring_lock')

            elif evname == 'tapdisk:ring_dispatch':
                q = fields['queue']
                rec = ring_open.pop(q, None)
                if rec is not None and rec['t1'] is not None:
                    n = fields.get('n_reqs')
                    meta(rec['cpu1'], TID_RING, f"CPU {rec['cpu1']}", "ring")
                    emit(f"ring: drain ({n})", 'X', rec['cpu1'], TID_RING,
                         rec['t1'], dur=t - rec['t1'],
                         args={'queue': q, 'n_reqs': n,
                               'us': round(us(t - rec['t1']), 3)},
                         cat='ring_drain')

    if truncated:
        print(f"  [output truncated: hit the size limit after "
              f"{nbytes} bytes of events; summary covers the emitted prefix "
              f"only]", file=sys.stderr)
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
    row("  phase guest copy",    buckets.get('cphase_guest_copy', []))
    row("  phase evtchn notify", buckets.get('cphase_evtchn', []))
    row("  phase kick wake",     buckets.get('cphase_kick_wake', []))
    row("  phase tapdisk kick",  buckets.get('cphase_kick', []))
    row("  phase qcow2 free",    buckets.get('cphase_free', []))
    row("  phase qcow2 kick+free", buckets.get('cphase_qcow2_post', []))
    row("  phase partial",       buckets.get('cphase_partial', []))
    row("deferred completion",   buckets.get('defcomp', []))
    row("comutex wait",    buckets.get('comutex', []))
    row("sched prepare",   buckets.get('sched_prepare', []))
    row("sched select",    buckets.get('sched_select', []))
    row("sched check",     buckets.get('sched_check', []))
    row("sched dispatch",  buckets.get('sched_dispatch', []))
    row("sched gc+unlock", buckets.get('sched_post', []))
    row("sched event cb",  buckets.get('sched_event', []))
    row("ring mutex wait", buckets.get('ring_lock', []))
    row("ring drain",      buckets.get('ring_drain', []))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    import argparse

    ap = argparse.ArgumentParser(
        description="Convert an LTTng text trace to a Perfetto JSON trace.")
    ap.add_argument('trace', help="babeltrace2 text trace file")
    ap.add_argument('-m', '--max-size', type=parse_size, default=None,
                    metavar='SIZE',
                    help="cap the output size (e.g. 500000, 512k, 10MB, 1G); "
                         "events past the limit are dropped, keeping a valid "
                         "(truncated) trace")
    args = ap.parse_args()

    PREFIX = '{"traceEvents":['
    SUFFIX = '],"displayTimeUnit":"ms"}\n'

    max_bytes = None
    if args.max_size is not None:
        max_bytes = args.max_size - len(PREFIX) - len(SUFFIX)
        if max_bytes <= 0:
            ap.error(f"--max-size must exceed the {len(PREFIX) + len(SUFFIX)} "
                     f"byte JSON envelope")

    out = sys.stdout
    out.write(PREFIX)
    buckets = convert(args.trace, out, max_bytes)
    out.write(SUFFIX)
    summarise(buckets)
