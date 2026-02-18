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
		uint64_t, req_id,
		uint8_t, operation,
		uint8_t, nr_segments,
		uint64_t, sector
	),
	TP_FIELDS(
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
		uint64_t, req_id,
		uint8_t, operation,
		uint64_t, sector
		//int, status
	),
	TP_FIELDS(
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
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, secs
	),
	TP_FIELDS(
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
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, secs
		//int, res
	),
	TP_FIELDS(
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, op, op)
		ctf_integer(uint64_t, sector, sector)
		ctf_integer(int, secs, secs)
		//ctf_integer(int, res, res)
	)
)

#else /* !HAVE_LTTNG */

#define tracepoint(...)

#endif /* HAVE_LTTNG */

#endif /* _TD_TRACEPOINTS_H */

#ifdef HAVE_LTTNG
#include <lttng/tracepoint-event.h>
#endif
