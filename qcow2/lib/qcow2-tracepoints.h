/*
 * QCOW2 block-layer tracepoint definitions (LTTng-UST).
 *
 * Separate provider ("qcow2") from the tapdisk one (drivers/td-tracepoints.h):
 * these live inside libqcow2 and instrument the part of the I/O path the
 * tapdisk provider cannot see — the read/write coroutines and the metadata
 * CoMutex (s->lock). Correlate with the tapdisk provider by timestamp (same
 * LTTng clock) and by offset (offset_bytes == sector << sector_shift).
 *
 * When built without --enable-lttng (HAVE_LTTNG undefined), tracepoint()
 * compiles to nothing and libqcow2 has no LTTng dependency.
 *
 * Usage:
 *   Build with: ./configure --enable-lttng && make
 *   Enable with: lttng enable-event --userspace 'qcow2:*'
 */

#include "config.h"

#ifdef HAVE_LTTNG
#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER qcow2
#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "qcow2-tracepoints.h"
#endif

#if !defined(_QCOW2_TRACEPOINTS_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _QCOW2_TRACEPOINTS_H

#ifdef HAVE_LTTNG

#include <lttng/tracepoint.h>
#include <stdint.h>

#define QCOW2_TP_PROVIDER	qcow2

/*
 * Read coroutine (qcow2_co_preadv_part) span.
 * Pair read_enter/read_return by co; correlate to tapdisk by offset.
 */
TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	read_enter,
	TP_ARGS(
		uint64_t, co,
		int64_t, offset,
		int64_t, bytes
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, co, co)
		ctf_integer(int64_t, offset, offset)
		ctf_integer(int64_t, bytes, bytes)
	)
)

TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	read_return,
	TP_ARGS(
		uint64_t, co,
		int64_t, offset,
		int, ret
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, co, co)
		ctf_integer(int64_t, offset, offset)
		ctf_integer(int, ret, ret)
	)
)

/*
 * Write coroutine (qcow2_co_pwritev_part) span.
 */
TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	write_enter,
	TP_ARGS(
		uint64_t, co,
		int64_t, offset,
		int64_t, bytes
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, co, co)
		ctf_integer(int64_t, offset, offset)
		ctf_integer(int64_t, bytes, bytes)
	)
)

TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	write_return,
	TP_ARGS(
		uint64_t, co,
		int64_t, offset,
		int, ret
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, co, co)
		ctf_integer(int64_t, offset, offset)
		ctf_integer(int, ret, ret)
	)
)

/*
 * Metadata CoMutex (s->lock) on the I/O path.
 *   lock_wait     : about to call qemu_co_mutex_lock(&s->lock)
 *   lock_acquired : lock obtained  (acquired - wait     = time blocked)
 *   lock_release  : about to unlock (release  - acquired = time held)
 * Pair by co; offset identifies the cluster being looked up/allocated.
 */
TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	lock_wait,
	TP_ARGS(
		uint64_t, co,
		int64_t, offset
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, co, co)
		ctf_integer(int64_t, offset, offset)
	)
)

TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	lock_acquired,
	TP_ARGS(
		uint64_t, co,
		int64_t, offset
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, co, co)
		ctf_integer(int64_t, offset, offset)
	)
)

TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	lock_release,
	TP_ARGS(
		uint64_t, co,
		int64_t, offset
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, co, co)
		ctf_integer(int64_t, offset, offset)
	)
)

/*
 * Block-layer completion callback (blk_aio_complete -> acb->common.cb), which
 * is where the user's completion callback runs (bench_complete in qcow2-bench,
 * qcow2_complete -> signal_completion in tapdisk). Brackets that call to
 * measure the time spent *in the callback*. Covers read/write/flush since they
 * all funnel through blk_aio_complete.
 *   req    = the callback opaque (the request object), pairs enter/return
 *   offset = byte offset (0 for flush)
 *   ret    = result delivered to the callback (on enter)
 */
TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	complete_enter,
	TP_ARGS(
		uint64_t, req,
		int64_t, offset,
		int, ret
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, req, req)
		ctf_integer(int64_t, offset, offset)
		ctf_integer(int, ret, ret)
	)
)

TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	complete_return,
	TP_ARGS(
		uint64_t, req,
		int64_t, offset
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, req, req)
		ctf_integer(int64_t, offset, offset)
	)
)

/*
 * Generic CoMutex contention (qemu_co_mutex_lock/unlock), emitted only on the
 * contended path so the volume tracks contention, not total acquisitions.
 * Unlike the qcow2 lock_* events above, this covers *every* CoMutex in
 * libqcow2 (s->lock, refcount cache, throttling, ...), identified by its
 * address.
 *
 *   comutex_wait     : self hit a held lock; holder = coroutine holding it,
 *                      holder_ctx = its AioContext (== self_ctx -> same thread,
 *                      != -> cross-iothread). This says *why* you wait: follow
 *                      `holder` to see what that coroutine is doing.
 *   comutex_acquired : self finally took it (acquired - wait = time blocked).
 *   comutex_release  : a holder dropped a lock that had waiters queued
 *                      (release - the waiter's wait = handoff latency).
 * Pair wait/acquired by (self, mutex).
 */
TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	comutex_wait,
	TP_ARGS(
		uint64_t, self,
		uint64_t, mutex,
		uint64_t, holder,
		uint64_t, holder_ctx,
		uint64_t, self_ctx
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, self, self)
		ctf_integer_hex(uint64_t, mutex, mutex)
		ctf_integer_hex(uint64_t, holder, holder)
		ctf_integer_hex(uint64_t, holder_ctx, holder_ctx)
		ctf_integer_hex(uint64_t, self_ctx, self_ctx)
	)
)

TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	comutex_acquired,
	TP_ARGS(
		uint64_t, self,
		uint64_t, mutex
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, self, self)
		ctf_integer_hex(uint64_t, mutex, mutex)
	)
)

TRACEPOINT_EVENT(
	QCOW2_TP_PROVIDER,
	comutex_release,
	TP_ARGS(
		uint64_t, self,
		uint64_t, mutex
	),
	TP_FIELDS(
		ctf_integer_hex(uint64_t, self, self)
		ctf_integer_hex(uint64_t, mutex, mutex)
	)
)

#else /* !HAVE_LTTNG */

#define tracepoint(...)

#endif /* HAVE_LTTNG */

#endif /* _QCOW2_TRACEPOINTS_H */

#ifdef HAVE_LTTNG
#include <lttng/tracepoint-event.h>
#endif
