/*
 * Tapdisk tracepoint definitions.
 *
 * When built with --enable-lttng, provides LTTng-UST tracepoints.
 * Otherwise, tracepoint() calls compile to nothing.
 *
 * Usage:
 *   Build with: ./configure --enable-lttng && make
 *   Trace with: tools/lttng-tapdisk.sh start
 */

#include "config.h"

/*
 * Phase markers for the begin/end "phase" field of bracketing tracepoints
 * (sched_*, aio_submit, guest_copy, grant_copy, evtchn_notify, ...).
 *
 * They use ASCII magic values rather than 0/1 so a raw trace dump reads
 * unambiguously.
 */
#ifndef TP_PHASE_BEGIN
#define TP_PHASE_BEGIN 0x4245474e /* "BEGN" */
#define TP_PHASE_END   0x454e4421 /* "END!" */
#endif

#ifdef HAVE_LTTNG
#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER tapdisk
#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "td-tracepoints.h"
#endif

#if !defined(_TD_TRACEPOINTS_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _TD_TRACEPOINTS_H

#ifdef HAVE_LTTNG

#include <lttng/tracepoint.h>
#include <stdint.h>

#define TAPDISK_TP_PROVIDER	tapdisk

/*
 * Emitted when a request is pulled from the shared ring.
 * Correlate with response_push using (queue, req_id).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	request_pull,	/* name */
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		uint8_t, operation,
		uint8_t, nr_segments,
		uint64_t, sector
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(uint8_t, operation, operation)
		ctf_integer(uint8_t, nr_segments, nr_segments)
		ctf_integer(uint64_t, sector, sector)
	)
)

/*
 * Emitted when a response is written to the shared ring.
 * Correlate with request_pull using (queue, req_id).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	response_push,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		uint8_t, operation,
		uint64_t, sector
		//int, status
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(uint8_t, operation, operation)
		ctf_integer(uint64_t, sector, sector)
		//ctf_integer(int, status, status)
	)
)

/*
 * Emitted when a request is dispatched to the storage driver (read or write).
 * Correlate with request_pull/response_push using req_id.
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	driver_queue,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, secs
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, op, op)
		ctf_integer(uint64_t, sector, sector)
		ctf_integer(int, secs, secs)
	)
)

/*
 * Emitted when the storage driver completes a request.
 * Correlate with driver_queue/request_pull/response_push using req_id.
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	driver_complete,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, secs
		//int, res
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, op, op)
		ctf_integer(uint64_t, sector, sector)
		ctf_integer(int, secs, secs)
		//ctf_integer(int, res, res)
	)
)

/*
 * Bracket the heavy work inside the qcow2 driver's completion callback
 * (signal_completion), once a request has fully completed at the block layer
 * and is being pushed back up the tapdisk/vbd chain.
 *
 *   complete_vbd_enter  : aio_inflight accounting done, about to call
 *                         td_complete_request (the vbd cb chain / grant copy).
 *   complete_vbd_return : td_complete_request has returned; the ring kick
 *                         (tapdisk_vbd_kick) follows. Carries the queue's
 *                         requests_inflight (this request still counted), i.e.
 *                         the value the wake watermark is tested against.
 *   complete_kicked     : tapdisk_vbd_kick done, about to free the request.
 *
 * Combined with the qcow2 provider's complete_enter/complete_return (which
 * bracket the whole callback), these split it into four phases:
 *   complete_enter -> complete_vbd_enter  : s->lock + aio_inflight accounting
 *   complete_vbd_enter -> complete_vbd_return : td_complete_request
 *   complete_vbd_return -> complete_kicked : tapdisk_vbd_kick (drain + wake)
 *   complete_kicked -> complete_return     : free_qcow2_request
 * Pair with driver_queue/driver_complete by (queue, req_id).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	complete_vbd_enter,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
	)
)

TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	complete_vbd_return,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		int, inflight
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, inflight, inflight)
	)
)

TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	complete_kicked,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
	)
)

/*
 * Break tapdisk_vbd_kick() down into its phases:
 *   kick_enter   : function entry (scheduler_kick = was a producer wake asked).
 *   kick_locked  : queue->mutex acquired (enter -> locked = mutex wait).
 *   kick_drained : completed_requests drained, before unlock
 *                  (locked -> drained = response/grant-copy work; drained =
 *                  number of vbd requests completed in this kick).
 *   kick_return  : function exit (drained -> return = unlock + the eventfd
 *                  write when woke = 1).
 * Pair by queue; all four run on the same thread in order.
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	kick_enter,
	TP_ARGS(
		uint16_t, queue,
		int, scheduler_kick
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(int, scheduler_kick, scheduler_kick)
	)
)

TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	kick_locked,
	TP_ARGS(
		uint16_t, queue
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
	)
)

TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	kick_drained,
	TP_ARGS(
		uint16_t, queue,
		int, drained
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(int, drained, drained)
	)
)

TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	kick_return,
	TP_ARGS(
		uint16_t, queue,
		int, woke
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(int, woke, woke)
	)
)

/*
 * Inside the kick drain loop, one token group per iteration:
 *   kick_loop     : top of the while() body (start of a token group).
 *   kick_final_cb : just before the final cb() of the group (the final=true
 *                   call, which usually pushes the response / notifies guest).
 * kick_loop -> kick_final_cb spans the intermediate same-token cbs;
 * kick_final_cb -> next kick_loop (or kick_drained) spans the final cb.
 * Pair by queue.
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	kick_loop,
	TP_ARGS(
		uint16_t, queue
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
	)
)

TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	kick_final_cb,
	TP_ARGS(
		uint16_t, queue
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
	)
)

/*
 * Emitted around the event channel notification to the guest.
 * phase: TP_PHASE_BEGIN (before xenevtchn_notify), TP_PHASE_END (after).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	evtchn_notify,
	TP_ARGS(
		uint16_t, queue,
		int, phase
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(int, phase, phase)
	)
)

/*
 * Emitted when a VBD request is issued from the new_requests queue to the
 * driver chain (moved to pending_requests).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	vbd_issue,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, nr_iov
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, op, op)
		ctf_integer(uint64_t, sector, sector)
		ctf_integer(int, nr_iov, nr_iov)
	)
)


/*
 * Emitted around the full guest_copy2() call (grant copy + segment handling).
 * phase: TP_PHASE_BEGIN, TP_PHASE_END.
 * direction: 0 = from guest (write path), 1 = to guest (read completion).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	guest_copy,
	TP_ARGS(
		uint64_t, req_id,
		int, phase,
		int, direction,
		int, nr_segments
	),
	TP_FIELDS(
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, phase, phase)
		ctf_integer(int, direction, direction)
		ctf_integer(int, nr_segments, nr_segments)
	)
)

/*
 * Emitted around the grant-copy ioctl (Xen hypercall).
 * phase: TP_PHASE_BEGIN (before ioctl), TP_PHASE_END (after ioctl).
 * direction: 0 = from guest (write path), 1 = to guest (read completion).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	grant_copy,
	TP_ARGS(
		//uint16_t, queue,
		uint64_t, req_id,
		int, phase,
		int, direction,
		int, nr_segments
	),
	TP_FIELDS(
		//ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, phase, phase)
		ctf_integer(int, direction, direction)
		ctf_integer(int, nr_segments, nr_segments)
	)
)

/*
 * Emitted around backend submit_all calls (wraps io_submit / lio_listio).
 * phase: TP_PHASE_BEGIN, TP_PHASE_END.
 * rw: 0 = read-write backend, 1 = read-only backend.
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	aio_submit,
	TP_ARGS(
		uint16_t, queue,
		int, phase,
		int, rw
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(int, phase, phase)
		ctf_integer(int, rw, rw)
	)
)

#else /* !HAVE_LTTNG */

#define tracepoint(...)

#endif /* HAVE_LTTNG */

#endif /* _TD_TRACEPOINTS_H */

#ifdef HAVE_LTTNG
#include <lttng/tracepoint-event.h>
#endif
