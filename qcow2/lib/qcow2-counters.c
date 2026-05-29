/*
 * qcow2-counters: see qemu/qcow2-counters.h for the rationale.
 */
#include "qemu/osdep.h"
#include "qemu/qcow2-counters.h"

#include <inttypes.h>

uint64_t qcow2_counter_thread_pool_submit;
uint64_t qcow2_counter_thread_pool_complete;
uint64_t qcow2_counter_cache_get;
uint64_t qcow2_counter_cache_get_read;
uint64_t qcow2_counter_cache_get_replace_entry;
uint64_t qcow2_counter_coroutine_yield;
uint64_t qcow2_counter_coroutine_enter;
uint64_t qcow2_counter_coroutine_terminate;
uint64_t qcow2_counter_co_mutex_lock_entry;
uint64_t qcow2_counter_co_mutex_lock_return;
uint64_t qcow2_counter_co_mutex_lock_uncontended;
uint64_t qcow2_counter_co_mutex_unlock_entry;
uint64_t qcow2_counter_co_mutex_unlock_return;
uint64_t qcow2_counter_aio_co_schedule;
uint64_t qcow2_counter_aio_co_schedule_bh_cb;
uint64_t qcow2_counter_bdrv_co_preadv;
uint64_t qcow2_counter_bdrv_co_pwritev;
uint64_t qcow2_counter_mutex_lock;
uint64_t qcow2_counter_mutex_locked;
uint64_t qcow2_counter_mutex_unlock;
uint64_t qcow2_counter_run_poll_handlers_begin;
uint64_t qcow2_counter_run_poll_handlers_end;
uint64_t qcow2_counter_poll_grow;
uint64_t qcow2_counter_poll_shrink;
uint64_t qcow2_counter_lockcnt_fast_path_attempt;
uint64_t qcow2_counter_lockcnt_fast_path_success;
uint64_t qcow2_counter_lockcnt_futex_wait;

void qcow2_counters_dump(FILE *out)
{
    fprintf(out, "\n=== qcow2 counters ===\n");
#define DUMP(name)                                                       \
    fprintf(out, "  %-40s %20" PRIu64 "\n", #name,                       \
            __atomic_load_n(&qcow2_counter_##name, __ATOMIC_RELAXED))
    DUMP(bdrv_co_preadv);
    DUMP(bdrv_co_pwritev);
    DUMP(coroutine_enter);
    DUMP(coroutine_yield);
    DUMP(coroutine_terminate);
    DUMP(co_mutex_lock_entry);
    DUMP(co_mutex_lock_uncontended);
    DUMP(co_mutex_lock_return);
    DUMP(co_mutex_unlock_entry);
    DUMP(co_mutex_unlock_return);
    DUMP(aio_co_schedule);
    DUMP(aio_co_schedule_bh_cb);
    DUMP(cache_get);
    DUMP(cache_get_read);
    DUMP(cache_get_replace_entry);
    DUMP(thread_pool_submit);
    DUMP(thread_pool_complete);
    DUMP(mutex_lock);
    DUMP(mutex_locked);
    DUMP(mutex_unlock);
    DUMP(run_poll_handlers_begin);
    DUMP(run_poll_handlers_end);
    DUMP(poll_grow);
    DUMP(poll_shrink);
    DUMP(lockcnt_fast_path_attempt);
    DUMP(lockcnt_fast_path_success);
    DUMP(lockcnt_futex_wait);
#undef DUMP

    /* Derived stats */
    uint64_t e = __atomic_load_n(&qcow2_counter_co_mutex_lock_entry,
                                 __ATOMIC_RELAXED);
    uint64_t u = __atomic_load_n(&qcow2_counter_co_mutex_lock_uncontended,
                                 __ATOMIC_RELAXED);
    if (e > 0 && u <= e) {
        fprintf(out, "  %-40s %20.2f %%\n",
                "co_mutex contention rate", 100.0 * (double)(e - u) / e);
    }

    uint64_t cg = __atomic_load_n(&qcow2_counter_cache_get, __ATOMIC_RELAXED);
    uint64_t cr = __atomic_load_n(&qcow2_counter_cache_get_read,
                                  __ATOMIC_RELAXED);
    if (cg > 0) {
        fprintf(out, "  %-40s %20.2f %%\n",
                "cache miss rate", 100.0 * (double)cr / cg);
    }

    uint64_t ml = __atomic_load_n(&qcow2_counter_mutex_lock, __ATOMIC_RELAXED);
    uint64_t mk = __atomic_load_n(&qcow2_counter_mutex_locked, __ATOMIC_RELAXED);
    if (ml > 0) {
        /* mutex_lock counts attempts (uncontended path increments only here);
         * mutex_locked counts the slow path that had to wait. */
        fprintf(out, "  %-40s %20.2f %%\n",
                "thread mutex slow-path rate",
                100.0 * (double)mk / ml);
    }

    uint64_t la = __atomic_load_n(&qcow2_counter_lockcnt_fast_path_attempt,
                                  __ATOMIC_RELAXED);
    uint64_t ls = __atomic_load_n(&qcow2_counter_lockcnt_fast_path_success,
                                  __ATOMIC_RELAXED);
    if (la > 0) {
        fprintf(out, "  %-40s %20.2f %%\n",
                "lockcnt fast-path success rate",
                100.0 * (double)ls / la);
    }
}

void qcow2_counters_reset(void)
{
#define ZERO(name) __atomic_store_n(&qcow2_counter_##name, 0, __ATOMIC_RELAXED)
    ZERO(bdrv_co_preadv);
    ZERO(bdrv_co_pwritev);
    ZERO(coroutine_enter);
    ZERO(coroutine_yield);
    ZERO(coroutine_terminate);
    ZERO(co_mutex_lock_entry);
    ZERO(co_mutex_lock_uncontended);
    ZERO(co_mutex_lock_return);
    ZERO(co_mutex_unlock_entry);
    ZERO(co_mutex_unlock_return);
    ZERO(aio_co_schedule);
    ZERO(aio_co_schedule_bh_cb);
    ZERO(cache_get);
    ZERO(cache_get_read);
    ZERO(cache_get_replace_entry);
    ZERO(thread_pool_submit);
    ZERO(thread_pool_complete);
    ZERO(mutex_lock);
    ZERO(mutex_locked);
    ZERO(mutex_unlock);
    ZERO(run_poll_handlers_begin);
    ZERO(run_poll_handlers_end);
    ZERO(poll_grow);
    ZERO(poll_shrink);
    ZERO(lockcnt_fast_path_attempt);
    ZERO(lockcnt_fast_path_success);
    ZERO(lockcnt_futex_wait);
#undef ZERO
}
