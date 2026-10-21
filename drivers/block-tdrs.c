/*
 * Copyright (c) 2026, Vates.
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * tdrs: qcow2 chains served by the blktap-rs driver (Rust, libtdrs), run by
 * the IO threads of tapdisk themselves.
 *
 * Unlike block-qcow2.c, which hands the requests to one qemu IOThread per
 * queue and gets them back from it (TD_DRIVER_THREADED), there is no thread
 * hop: each IO thread has its own tdrs queue, created on its first request.
 * The requests issued during an iteration of the thread are queued
 * (tdrs_submit) and started together at the end of the iteration (flush
 * hook, see tapdisk_server_register_io_flush()), with one io_submit for
 * their I/O. Their completions are polled by the scheduler of the thread
 * (the AIO eventfd of the queue) and returned on that thread, as the aio
 * driver does. The image chain is shared by the queues; the driver takes
 * its own locks (one per L2 table), and a queue waiting for a lock held by
 * another one is woken through its second eventfd.
 *
 * The image is given as "tdrs:<path>" for a qcow2 image, or
 * "tdrs:<type>:<path>" for another format blktap-rs reads (aio, ...). The
 * parents are opened by libtdrs: the driver has no parent for tapdisk.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tdrs.h>

#include "tapdisk.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "tapdisk-server.h"
#include "tapdisk-vbd.h"
#include "timeout-math.h"

/* requests in flight per queue, as block-qcow2.c */
#define TDRS_QUEUE_REQS TAPDISK_DATA_REQUESTS

struct tdrs_state;

struct tdrs_req {
	td_request_t                 treq;
	struct tdrs_queue_state     *qs;
};

struct tdrs_queue_state {
	struct tdrs_state           *s;
	td_queue_id_t                qid;

	/* IO thread only */
	struct tdrs_queue           *q;
	event_id_t                   aio_evid;
	event_id_t                   wake_evid;
	bool                         queued;     /* submitted since the last flush */
	struct tdrs_req             *reqs;
	struct tdrs_req            **free;
	int                          nr_free;

	/* teardown on the IO thread, waited for by td_close */
	pthread_mutex_t              lock;
	pthread_cond_t               cond;
	bool                         closed;
};

struct tdrs_state {
	struct tdrs_chain           *chain;
	struct tdrs_queue_state      queue[TAPDISK_MAX_VBD_THREADS];
	int                          inflight;   /* atomic */
};

static void
tdrs_complete(void *opaque, int err)
{
	struct tdrs_req *r = opaque;
	struct tdrs_queue_state *qs = r->qs;
	td_request_t treq = r->treq;

	qs->free[qs->nr_free++] = r;
	__atomic_sub_fetch(&qs->s->inflight, 1, __ATOMIC_RELAXED);
	td_complete_request(treq, err);
}

static void
tdrs_aio_event(event_id_t id, char mode, void *private)
{
	struct tdrs_queue_state *qs = private;

	tdrs_poll(qs->q, TDRS_EV_AIO);
}

static void
tdrs_wake_event(event_id_t id, char mode, void *private)
{
	struct tdrs_queue_state *qs = private;

	tdrs_poll(qs->q, TDRS_EV_WAKE);
}

/* end of an IO thread iteration: start the requests it queued */
static void
tdrs_flush(void *arg)
{
	struct tdrs_queue_state *qs = arg;

	if (qs->queued) {
		qs->queued = false;
		tdrs_poll(qs->q, 0);
	}
}

/* on the IO thread of the queue, on its first request */
static int
tdrs_queue_init(struct tdrs_queue_state *qs)
{
	int aio_fd, wake_fd, i, err;

	qs->reqs = calloc(TDRS_QUEUE_REQS, sizeof(*qs->reqs));
	qs->free = calloc(TDRS_QUEUE_REQS, sizeof(*qs->free));
	if (!qs->reqs || !qs->free) {
		err = -ENOMEM;
		goto fail;
	}
	for (i = 0; i < TDRS_QUEUE_REQS; i++) {
		qs->reqs[i].qs = qs;
		qs->free[i] = &qs->reqs[i];
	}
	qs->nr_free = TDRS_QUEUE_REQS;

	qs->q = tdrs_queue_new(qs->s->chain, TDRS_QUEUE_REQS, tdrs_complete);
	if (!qs->q) {
		err = -ENOMEM;
		goto fail;
	}
	tdrs_queue_fds(qs->q, &aio_fd, &wake_fd);

	qs->aio_evid = tapdisk_server_register_io_event(qs->qid, SCHEDULER_POLL_READ_FD,
							aio_fd, TV_INF, tdrs_aio_event, qs);
	qs->wake_evid = tapdisk_server_register_io_event(qs->qid, SCHEDULER_POLL_READ_FD,
							 wake_fd, TV_INF, tdrs_wake_event, qs);
	if (qs->aio_evid < 0 || qs->wake_evid < 0) {
		err = -EINVAL;
		goto fail;
	}
	err = tapdisk_server_register_io_flush(qs->qid, tdrs_flush, qs);
	if (err)
		goto fail;

	DPRINTF("tdrs: queue %u ready (aio fd %d, wake fd %d)\n", qs->qid, aio_fd, wake_fd);
	return 0;

fail:
	EPRINTF("tdrs: queue %u: %s\n", qs->qid, strerror(-err));
	if (qs->aio_evid > 0)
		tapdisk_server_unregister_io_event(qs->qid, qs->aio_evid);
	if (qs->wake_evid > 0)
		tapdisk_server_unregister_io_event(qs->qid, qs->wake_evid);
	qs->aio_evid = qs->wake_evid = 0;
	if (qs->q)
		tdrs_queue_free(qs->q);
	qs->q = NULL;
	free(qs->reqs);
	free(qs->free);
	qs->reqs = NULL;
	qs->free = NULL;
	return err;
}

/* on the IO thread of the queue, from td_close */
static void
tdrs_queue_close(event_id_t id, char mode, void *private)
{
	struct tdrs_queue_state *qs = private;

	tapdisk_server_unregister_io_event(qs->qid, id);

	if (qs->q) {
		tapdisk_server_unregister_io_flush(qs->qid, tdrs_flush, qs);
		tapdisk_server_unregister_io_event(qs->qid, qs->aio_evid);
		tapdisk_server_unregister_io_event(qs->qid, qs->wake_evid);
		if (tdrs_busy(qs->q))
			EPRINTF("tdrs: queue %u closed with %u requests in flight\n",
				qs->qid, tdrs_busy(qs->q));
		else
			tdrs_queue_free(qs->q);
		qs->q = NULL;
		free(qs->reqs);
		free(qs->free);
	}

	pthread_mutex_lock(&qs->lock);
	qs->closed = true;
	pthread_cond_signal(&qs->cond);
	pthread_mutex_unlock(&qs->lock);
}

static void
tdrs_queue(td_driver_t *driver, td_request_t treq, int op)
{
	struct tdrs_state *s = driver->data;
	struct tdrs_queue_state *qs;
	struct tdrs_req *r;
	td_queue_id_t qid;
	int err;

	qid = treq.vreq->vqueue - treq.vreq->vqueue->vbd->queues;
	if (qid >= TAPDISK_MAX_VBD_THREADS || !tapdisk_server_in_io_thread(qid)) {
		EPRINTF("tdrs: request of queue %u off its IO thread\n", qid);
		err = -EIO;
		goto fail;
	}

	qs = &s->queue[qid];
	if (!qs->q) {
		err = tdrs_queue_init(qs);
		if (err)
			goto fail;
	}
	if (!qs->nr_free) {
		err = -EBUSY;
		goto fail;
	}

	r = qs->free[--qs->nr_free];
	r->treq = treq;
	err = tdrs_submit(qs->q, op, treq.sec, treq.secs, treq.buf, r);
	if (err) {
		qs->free[qs->nr_free++] = r;
		goto fail;
	}
	qs->queued = true;
	__atomic_add_fetch(&s->inflight, 1, __ATOMIC_RELAXED);
	return;

fail:
	td_complete_request(treq, err);
}

static void
tdrs_queue_read(td_driver_t *driver, td_request_t treq)
{
	tdrs_queue(driver, treq, TDRS_OP_READ);
}

static void
tdrs_queue_write(td_driver_t *driver, td_request_t treq)
{
	tdrs_queue(driver, treq, TDRS_OP_WRITE);
}

static int
tdrs_open_image(td_driver_t *driver, const char *name,
		struct td_vbd_encryption *encryption, td_flag_t flags)
{
	struct tdrs_state *s = driver->data;
	struct tdrs_info info;
	const char *colon, *slash;
	char *params;
	int err, qid;

	/* "<path>" is a qcow2 image, "<type>:<path>" anything libtdrs opens */
	colon = strchr(name, ':');
	slash = strchr(name, '/');
	if (colon && (!slash || colon < slash))
		err = asprintf(&params, "%s", name);
	else
		err = asprintf(&params, "qcow2:%s", name);
	if (err < 0)
		return -ENOMEM;

	memset(s, 0, sizeof(*s));
	err = tdrs_open(params, flags, &s->chain, &info);
	if (err) {
		EPRINTF("tdrs: opening %s: %s\n", params, strerror(-err));
		free(params);
		return err;
	}

	for (qid = 0; qid < TAPDISK_MAX_VBD_THREADS; qid++) {
		struct tdrs_queue_state *qs = &s->queue[qid];

		qs->s = s;
		qs->qid = qid;
		pthread_mutex_init(&qs->lock, NULL);
		pthread_cond_init(&qs->cond, NULL);
	}

	driver->info.size = info.size;
	driver->info.sector_size = info.sector_size;
	driver->info.info = info.info;

	DPRINTF("tdrs: opened %s: %" PRIu64 " sectors\n", params, info.size);
	free(params);
	return 0;
}

static int
tdrs_close_image(td_driver_t *driver)
{
	struct tdrs_state *s = driver->data;
	int qid;

	/* each queue is torn down on its IO thread */
	for (qid = 0; qid < TAPDISK_MAX_VBD_THREADS; qid++) {
		struct tdrs_queue_state *qs = &s->queue[qid];

		if (qs->q) {
			event_id_t id;

			qs->closed = false;
			id = tapdisk_server_register_io_event(qid, SCHEDULER_POLL_TIMEOUT, -1,
							      TV_ZERO, tdrs_queue_close, qs);
			if (id < 0) {
				EPRINTF("tdrs: cannot close queue %d: %s\n", qid, strerror(-id));
			} else {
				tapdisk_server_io_scheduler_wake(qid);
				pthread_mutex_lock(&qs->lock);
				while (!qs->closed)
					pthread_cond_wait(&qs->cond, &qs->lock);
				pthread_mutex_unlock(&qs->lock);
			}
		}
		pthread_mutex_destroy(&qs->lock);
		pthread_cond_destroy(&qs->cond);
	}

	tdrs_close(s->chain);
	s->chain = NULL;
	return 0;
}

static int
tdrs_get_parent_id(td_driver_t *driver, td_disk_id_t *id)
{
	/* the parents are opened by libtdrs */
	return TD_NO_PARENT;
}

static int
tdrs_validate_parent(td_driver_t *driver, td_driver_t *pdriver, td_flag_t flags)
{
	return -EINVAL;
}

static int
tdrs_pending(td_driver_t *driver)
{
	struct tdrs_state *s = driver->data;

	return __atomic_load_n(&s->inflight, __ATOMIC_RELAXED) > 0;
}

static void
tdrs_debug(td_driver_t *driver)
{
	struct tdrs_state *s = driver->data;

	DPRINTF("tdrs: %d requests in flight\n",
		__atomic_load_n(&s->inflight, __ATOMIC_RELAXED));
}

struct tap_disk tapdisk_tdrs = {
	.disk_type          = "tapdisk_tdrs",
	.flags              = TD_DRIVER_MULTIQUEUE,
	.private_data_size  = sizeof(struct tdrs_state),
	.td_open            = tdrs_open_image,
	.td_close           = tdrs_close_image,
	.td_queue_read      = tdrs_queue_read,
	.td_queue_write     = tdrs_queue_write,
	.td_get_parent_id   = tdrs_get_parent_id,
	.td_validate_parent = tdrs_validate_parent,
	.td_pending         = tdrs_pending,
	.td_debug           = tdrs_debug,
	/* not implemented */
	.td_queue_block_status = NULL,
	.td_stats           = NULL,
	.td_commit          = NULL,
	.td_query_commit_job = NULL,
	.td_cancel_commit_job = NULL,
	.td_rrd             = NULL,
};
