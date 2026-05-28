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
extern uint64_t qcow2_counter_co_mutex_lock_entry;
extern uint64_t qcow2_counter_co_mutex_lock_return;
extern uint64_t qcow2_counter_co_mutex_lock_uncontended;
extern uint64_t qcow2_counter_aio_co_schedule;
extern uint64_t qcow2_counter_aio_co_schedule_bh_cb;
extern uint64_t qcow2_counter_bdrv_co_preadv;
extern uint64_t qcow2_counter_bdrv_co_pwritev;

/*
 * Relaxed ordering: we only need eventual visibility for the final dump.
 * No happens-before constraints with surrounding ops.
 */
#define QCOW2_COUNTER_INC(name) \
    __atomic_add_fetch(&qcow2_counter_##name, 1, __ATOMIC_RELAXED)

void qcow2_counters_dump(FILE *out);
void qcow2_counters_reset(void);

#endif /* QCOW2_COUNTERS_H */
