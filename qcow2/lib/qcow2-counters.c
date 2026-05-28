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
uint64_t qcow2_counter_co_mutex_lock_entry;
uint64_t qcow2_counter_co_mutex_lock_return;
uint64_t qcow2_counter_co_mutex_lock_uncontended;
uint64_t qcow2_counter_aio_co_schedule;
uint64_t qcow2_counter_aio_co_schedule_bh_cb;
uint64_t qcow2_counter_bdrv_co_preadv;
uint64_t qcow2_counter_bdrv_co_pwritev;

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
    DUMP(co_mutex_lock_entry);
    DUMP(co_mutex_lock_uncontended);
    DUMP(co_mutex_lock_return);
    DUMP(aio_co_schedule);
    DUMP(aio_co_schedule_bh_cb);
    DUMP(cache_get);
    DUMP(cache_get_read);
    DUMP(cache_get_replace_entry);
    DUMP(thread_pool_submit);
    DUMP(thread_pool_complete);
#undef DUMP

    /* Derived stats */
    uint64_t e = __atomic_load_n(&qcow2_counter_co_mutex_lock_entry,
                                 __ATOMIC_RELAXED);
    uint64_t u = __atomic_load_n(&qcow2_counter_co_mutex_lock_uncontended,
                                 __ATOMIC_RELAXED);
    if (e > 0) {
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
}

void qcow2_counters_reset(void)
{
#define ZERO(name) __atomic_store_n(&qcow2_counter_##name, 0, __ATOMIC_RELAXED)
    ZERO(bdrv_co_preadv);
    ZERO(bdrv_co_pwritev);
    ZERO(coroutine_enter);
    ZERO(coroutine_yield);
    ZERO(co_mutex_lock_entry);
    ZERO(co_mutex_lock_uncontended);
    ZERO(co_mutex_lock_return);
    ZERO(aio_co_schedule);
    ZERO(aio_co_schedule_bh_cb);
    ZERO(cache_get);
    ZERO(cache_get_read);
    ZERO(cache_get_replace_entry);
    ZERO(thread_pool_submit);
    ZERO(thread_pool_complete);
#undef ZERO
}
