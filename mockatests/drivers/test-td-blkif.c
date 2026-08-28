/*
 * Copyright (c) 2026, Vates
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "test-suites.h"

#include "td-blkif.h"
#include "timeout-math.h"

#include <stdlib.h>
#include <xen/io/blkif.h>

/*
 * The ring check event is the only thing that brings tapdisk back to a ring it
 * left requests in: when we stop consuming the ring mid-way (barrier request,
 * or no free request slot) front-end notifications are not re-armed, so a lost
 * ring check request stalls the ring for good. Completions may run in a driver
 * thread (TD_DRIVER_THREADED, e.g. qcow2) and request a ring check while the
 * main thread is unscheduling it, hence blkif->chkrng_pending.
 */

static struct timeval last_timeout;
static int n_set_timeout;

int
__wrap_tapdisk_server_event_set_timeout(event_id_t event_id __attribute__((unused)),
		struct timeval timeo)
{
	last_timeout = timeo;
	n_set_timeout++;
	return 0;
}

static struct td_xenblkif *
blkif_fixture(void)
{
	static struct td_xenblkif blkif;

	memset(&blkif, 0, sizeof(blkif));
	blkif.chkrng_event = 1;

	last_timeout = TV_INF;
	n_set_timeout = 0;

	return &blkif;
}

void
test_blkif_sched_chkrng_arms_and_records_request(void **state __attribute__((unused)))
{
	struct td_xenblkif *blkif = blkif_fixture();

	tapdisk_xenblkif_sched_chkrng(blkif);

	assert_true(blkif->chkrng_pending);
	assert_int_equal(n_set_timeout, 1);
	assert_false(TV_IS_INF(last_timeout));
	assert_int_equal(last_timeout.tv_sec, 0);
	assert_int_equal(last_timeout.tv_usec, 0);
}

void
test_blkif_unsched_chkrng_disarms_when_no_request(void **state __attribute__((unused)))
{
	struct td_xenblkif *blkif = blkif_fixture();

	/* The ring check we are unscheduling has already been honoured. */
	blkif->chkrng_pending = false;

	tapdisk_xenblkif_unsched_chkrng(blkif);

	assert_int_equal(n_set_timeout, 1);
	assert_true(TV_IS_INF(last_timeout));
}

void
test_blkif_unsched_chkrng_keeps_pending_request(void **state __attribute__((unused)))
{
	struct td_xenblkif *blkif = blkif_fixture();

	/*
	 * A completion running in a driver thread requested a ring check that the
	 * main thread has not honoured yet. Unscheduling must not cancel it, or
	 * nothing will ever come back to the ring.
	 */
	blkif->chkrng_pending = true;

	tapdisk_xenblkif_unsched_chkrng(blkif);

	assert_int_equal(n_set_timeout, 2);
	assert_false(TV_IS_INF(last_timeout));
	assert_int_equal(last_timeout.tv_sec, 0);
	assert_int_equal(last_timeout.tv_usec, 0);
}

void
test_blkif_sched_chkrng_survives_unsched(void **state __attribute__((unused)))
{
	struct td_xenblkif *blkif = blkif_fixture();

	/* Main thread honours a ring check... */
	blkif->chkrng_pending = false;
	/* ...while a driver thread requests another one. */
	tapdisk_xenblkif_sched_chkrng(blkif);
	/* The main thread then unschedules the event it was dispatched for. */
	tapdisk_xenblkif_unsched_chkrng(blkif);

	assert_false(TV_IS_INF(last_timeout));
}


/*
 * The Xen ring uses an event-index (edge triggered) notification scheme: the
 * front-end only sends an event channel notification when its producer index
 * *crosses* sring->req_event, and only the back-end moves req_event forward,
 * via RING_FINAL_CHECK_FOR_REQUESTS -- which re-arms it only once the ring has
 * been completely drained.
 *
 * So when tapdisk stops consuming the ring half way (it stopped at a barrier
 * request, or it ran out of free request slots) the front-end goes *silent*:
 * no notification is lost or dropped, the guest simply stops sending any.
 * From that point on the only thing that can bring tapdisk back to the ring is
 * the ring check it schedules for itself, which is why losing one stalls the
 * ring for good.
 *
 * These tests pin that behaviour down, since it is the premise the ring check
 * machinery rests on.
 */

#define TEST_RING_PAGE_SIZE 4096

struct ring_fixture {
	blkif_sring_t      *sring;
	blkif_front_ring_t  front;
	blkif_back_ring_t   back;
};

static void
ring_fixture_init(struct ring_fixture *r)
{
	r->sring = malloc(TEST_RING_PAGE_SIZE);
	assert_non_null(r->sring);

	SHARED_RING_INIT(r->sring);
	FRONT_RING_INIT(&r->front, r->sring, TEST_RING_PAGE_SIZE);
	BACK_RING_INIT(&r->back, r->sring, TEST_RING_PAGE_SIZE);
}

static void
ring_fixture_free(struct ring_fixture *r)
{
	free(r->sring);
	r->sring = NULL;
}

/* The guest queues @n requests and tells us whether it notified the back-end */
static int
guest_push_requests(struct ring_fixture *r, unsigned int n)
{
	int notify;

	r->front.req_prod_pvt += n;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&r->front, notify);

	return notify;
}

void
test_ring_notifies_when_backend_is_drained(void **state __attribute__((unused)))
{
	struct ring_fixture r;

	ring_fixture_init(&r);

	/* A freshly initialised ring is armed, so the first batch notifies. */
	assert_true(guest_push_requests(&r, 4));

	ring_fixture_free(&r);
}

void
test_ring_goes_silent_when_backend_stops_early(void **state __attribute__((unused)))
{
	struct ring_fixture r;
	int work;

	ring_fixture_init(&r);

	assert_true(guest_push_requests(&r, 8));

	/*
	 * tapdisk consumes part of the batch and stops -- it hit a barrier request
	 * or ran out of free request slots -- so it never re-arms the ring.
	 */
	r.back.req_cons += 3;

	/* The guest keeps queueing, and the back-end is never told about it. */
	assert_false(guest_push_requests(&r, 8));
	assert_false(guest_push_requests(&r, 8));

	/*
	 * Even asking for the final check does not re-arm the ring while requests
	 * are left in it: RING_FINAL_CHECK_FOR_REQUESTS only moves req_event once
	 * there is no unconsumed request.
	 */
	RING_FINAL_CHECK_FOR_REQUESTS(&r.back, work);
	assert_true(work);
	assert_false(guest_push_requests(&r, 4));

	/* Requests are sitting in the ring with nobody on the way to collect them */
	assert_int_not_equal(r.sring->req_prod, r.back.req_cons);

	ring_fixture_free(&r);
}

void
test_ring_notifies_again_once_backend_drains_it(void **state __attribute__((unused)))
{
	struct ring_fixture r;
	int work;

	ring_fixture_init(&r);

	assert_true(guest_push_requests(&r, 8));
	r.back.req_cons += 3;
	assert_false(guest_push_requests(&r, 8));

	/* This is what the scheduled ring check ends up doing: drain, then re-arm */
	r.back.req_cons = r.sring->req_prod;
	RING_FINAL_CHECK_FOR_REQUESTS(&r.back, work);
	assert_false(work);

	/* The guest can notify us again */
	assert_true(guest_push_requests(&r, 1));

	ring_fixture_free(&r);
}
