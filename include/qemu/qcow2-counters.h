/*
 * qcow2-counters: lightweight atomic counters wired into selected trace_*
 * stubs from qemu/trace.h. Lets us quantitatively compare two qcow2-bench
 * runs (slow vs fast machine) without bpftrace/bcc.
 *
 * Add a new counter by:
 *   1. extern declaration here
 *   2. definition in qcow2-counters.c
 *   3. an entry in the DUMP() block in qcow2_counters_dump()
 *   4. swap the matching trace_* stub in include/qemu/trace.h:
 *        #define trace_thread_pool_submit(...) QCOW2_COUNTER_INC(thread_pool_submit)
 */
#ifndef QCOW2_COUNTERS_H
#define QCOW2_COUNTERS_H

#include <stdint.h>
#include <stdio.h>

extern uint64_t qcow2_counter_thread_pool_submit;
extern uint64_t qcow2_counter_thread_pool_complete;
extern uint64_t qcow2_counter_cache_get;
extern uint64_t qcow2_counter_cache_get_read;
extern uint64_t qcow2_counter_cache_get_replace_entry;
extern uint64_t qcow2_counter_coroutine_yield;
extern uint64_t qcow2_counter_coroutine_enter;
extern uint64_t qcow2_counter_coroutine_terminate;
extern uint64_t qcow2_counter_co_mutex_lock_entry;
extern uint64_t qcow2_counter_co_mutex_lock_return;
extern uint64_t qcow2_counter_co_mutex_lock_uncontended;
extern uint64_t qcow2_counter_co_mutex_unlock_entry;
extern uint64_t qcow2_counter_co_mutex_unlock_return;
/*
 * CoMutex adaptive-spin instrumentation (qemu_co_mutex_lock):
 *  - spin_enter:    contended acquisitions eligible to spin (waiters == 1)
 *  - spin_samectx:  spins skipped because the holder is on our AioContext
 *  - spin_acquired: spins that caught the release (re-took the fast path)
 *  - spin_iter:     total cpu_relax() iterations actually burned
 * spin_enter - spin_samectx = cross-context spins (the ones that burn CPU).
 */
extern uint64_t qcow2_counter_co_mutex_spin_enter;
extern uint64_t qcow2_counter_co_mutex_spin_samectx;
extern uint64_t qcow2_counter_co_mutex_spin_acquired;
extern uint64_t qcow2_counter_co_mutex_spin_iter;
extern uint64_t qcow2_counter_aio_co_schedule;
extern uint64_t qcow2_counter_aio_co_schedule_bh_cb;
extern uint64_t qcow2_counter_bdrv_co_preadv;
extern uint64_t qcow2_counter_bdrv_co_pwritev;
/* QemuMutex (thread mutex, pthread-based). Distinct from CoMutex above. */
extern uint64_t qcow2_counter_mutex_lock;
extern uint64_t qcow2_counter_mutex_locked;
extern uint64_t qcow2_counter_mutex_unlock;
/* AioContext event loop iterations and adaptive poll changes. */
extern uint64_t qcow2_counter_run_poll_handlers_begin;
extern uint64_t qcow2_counter_run_poll_handlers_end;
extern uint64_t qcow2_counter_poll_grow;
extern uint64_t qcow2_counter_poll_shrink;
/* QemuLockCnt — used inside aio_context to track in-flight callers. */
extern uint64_t qcow2_counter_lockcnt_fast_path_attempt;
extern uint64_t qcow2_counter_lockcnt_fast_path_success;
extern uint64_t qcow2_counter_lockcnt_futex_wait;

/*
 * Relaxed ordering: we only need eventual visibility for the final dump.
 * No happens-before constraints with surrounding ops.
 */
#define QCOW2_COUNTER_INC(name) \
    __atomic_add_fetch(&qcow2_counter_##name, 1, __ATOMIC_RELAXED)

/* Add a batch (e.g. a local spin iteration count) in one atomic op. */
#define QCOW2_COUNTER_ADD(name, n) \
    __atomic_add_fetch(&qcow2_counter_##name, (n), __ATOMIC_RELAXED)

void qcow2_counters_dump(FILE *out);
void qcow2_counters_reset(void);

#endif /* QCOW2_COUNTERS_H */
