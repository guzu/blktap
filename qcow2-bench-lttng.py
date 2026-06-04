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
    qcow2:complete_enter / complete_return -> slice on "CPU N / Completion" track
    qcow2:comutex_wait / comutex_acquired -> slice on "CoMutex contention" track
    qcow2:comutex_release              -> instant event on the holder's CPU track

Track layout (one Perfetto "process" per cpu_id, grouped in the UI):
    CPU N
      IO          -- read/write request spans
      Lock wait   -- time blocked waiting for s->lock
      Lock held   -- time holding s->lock (L2 lookup / alloc)
      Completion  -- time spent in the request completion callback
    CoMutex contention   -- rare generic CoMutex blocks (all mutexes, not just s->lock)
"""

import re
import sys
import json

# ---------------------------------------------------------------------------
# Regex
# ---------------------------------------------------------------------------
RE_LINE = re.compile(
    r'\[(\d{2}:\d{2}:\d{2})\.(\d{9})\]'   # [HH:MM:SS.nnnnnnnnn]
    r'\s+\(\+[^\)]+\)'                      # (+delta)
    r'\s+\S+'                               # hostname
    r'\s+(qcow2:\w+):'                      # event name
    r'\s+\{[^}]+\}'                         # context block  { cpu_id = N }
    r',\s+\{([^}]+)\}'                      # payload block  { k = v, ... }
)
RE_CPU = re.compile(r'cpu_id\s*=\s*(\d+)')
RE_KV  = re.compile(r'(\w+)\s*=\s*(0x[0-9A-Fa-f]+|-?\d+)')

# Within each CPU "process", virtual threads:
TID_IO       = 0
TID_LOCKWAIT = 1
TID_LOCKHELD = 2
TID_COMPLETE = 3
PID_COMUTEX  = 999


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

def convert(path: str) -> list:
    events = []
    meta_seen: set = set()

    # open slots keyed by coroutine address
    io_open:       dict[int, tuple[int, dict]] = {}
    lockwait_open: dict[int, tuple[int, dict]] = {}
    lockheld_open: dict[int, tuple[int, dict]] = {}
    cmwait_open:   dict[int, tuple[int, dict]] = {}
    complete_open: dict[int, tuple[int, dict]] = {}  # req -> (ts, fields)

    def emit(name, ph, pid, tid, t, dur=None, args=None, cat=None):
        e = {"name": name, "ph": ph, "pid": pid, "tid": tid, "ts": us(t)}
        if dur is not None:
            e["dur"] = us(dur)
        if args:
            e["args"] = args
        if cat:
            e["cat"] = cat
        events.append(e)

    def meta(pid, tid, pname, tname):
        key = (pid, tid)
        if key not in meta_seen:
            meta_seen.add(key)
            events.append({"ph": "M", "pid": pid, "tid": tid,
                           "name": "process_name", "args": {"name": pname}})
            events.append({"ph": "M", "pid": pid, "tid": tid,
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

            # ---- Completion callback span (blk_aio_complete -> user cb) -----
            elif evname == 'qcow2:complete_enter':
                req = fields['req']
                meta(pid, TID_COMPLETE, f"CPU {cpu}", "Completion")
                complete_open[req] = (t, {'offset': fields['offset'],
                                          'ret':    fields.get('ret', 0)})

            elif evname == 'qcow2:complete_return':
                req = fields['req']
                if req in complete_open:
                    t0, info = complete_open.pop(req)
                    dur = t - t0
                    emit("completion cb", 'X', pid, TID_COMPLETE, t0, dur=dur,
                         args={**info, 'cb_us': round(us(dur), 3)},
                         cat='complete')

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

    return events


# ---------------------------------------------------------------------------
# Stats summary to stderr
# ---------------------------------------------------------------------------

def summarise(events: list):
    import statistics as st
    buckets: dict[str, list] = {}
    for e in events:
        if e.get('ph') != 'X':
            continue
        cat = e.get('cat', '')
        dur = e.get('dur', 0.0)
        buckets.setdefault(cat, []).append(dur)

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
    row("io span (read)",  buckets.get('read', []))
    row("io span (write)", buckets.get('write', []))
    row("lock wait",       buckets.get('lock_wait', []))
    row("lock held",       buckets.get('lock_held', []))
    row("completion cb",   buckets.get('complete', []))
    row("comutex wait",    buckets.get('comutex', []))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} lttng-trace.log > trace.json",
              file=sys.stderr)
        sys.exit(1)

    events = convert(sys.argv[1])
    summarise(events)
    print(json.dumps({"traceEvents": events, "displayTimeUnit": "ms"},
                     separators=(',', ':')))
