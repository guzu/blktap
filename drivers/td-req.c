/*
 * Copyright (c) 2016, Citrix Systems, Inc.
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

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <syslog.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <string.h>
#include <alloca.h>

#ifdef __linux__
#include <linux/version.h>
#endif

#include "blktap-xenif.h"
#include "debug.h"
#include "td-req.h"
#include "td-blkif.h"
#include "td-ctx.h"
#include "tapdisk-server.h"
#include "tapdisk-metrics.h"
#include "tapdisk-vbd.h"
#include "tapdisk-log.h"
#include "tapdisk.h"
#include "timeout-math.h"
#include "util.h"

#ifdef DEBUG
#define BLKIF_MSG_POISON 0xdeadbeef
#endif

#define ERR(blkif, fmt, args...) \
    EPRINTF("%d/%d: "fmt, (blkif)->domid, (blkif)->devid, ##args);

#define TD_REQS_BUFCACHE_EXPIRE 3 // time in seconds
#define TD_REQS_BUFCACHE_MIN    1 // buffers to always keep in the cache

static void
td_xenblkif_bufcache_free(struct td_xenblkif * const blkif);
static inline void
td_xenblkif_bufcache_evt_unreg(struct td_xenblkif * const blkif);

static void
td_xenblkif_bufcache_event(event_id_t id, char mode, void *private)
{
    struct td_xenblkif *blkif = private;

    pthread_mutex_lock(&blkif->mutex);
    td_xenblkif_bufcache_free(blkif);

    td_xenblkif_bufcache_evt_unreg(blkif);
    pthread_mutex_unlock(&blkif->mutex);
}

/**
 * Unregister the event to expire the request buffer cache.
 *
 * @param blkif the block interface
 */
static inline void
td_xenblkif_bufcache_evt_unreg(struct td_xenblkif * const blkif)
{
    if (blkif->reqs_bufcache_evtid > 0){
        tapdisk_server_unregister_event(blkif->reqs_bufcache_evtid);
    }
    blkif->reqs_bufcache_evtid = 0;
}

/**
 * Register the event to expire the request buffer cache.
 *
 * @param blkif the block interface
 */
static inline void
td_xenblkif_bufcache_evt_reg(struct td_xenblkif * const blkif)
{
    blkif->reqs_bufcache_evtid =
        tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT,
                                      -1, /* dummy fd */
                                      TV_SECS(TD_REQS_BUFCACHE_EXPIRE),
                                      td_xenblkif_bufcache_event,
                                      blkif);
}

/**
 * Free request buffer cache.
 *
 * @param blkif the block interface
 */
static void
td_xenblkif_bufcache_free(struct td_xenblkif * const blkif)
{
    ASSERT(blkif);

    while (blkif->n_reqs_bufcache_free > TD_REQS_BUFCACHE_MIN){
        munmap(blkif->reqs_bufcache[--blkif->n_reqs_bufcache_free],
               (size_t)TD_REQ_BUFFER_SIZE);
    }
}

/**
 * Get buffer for a request. From cache if available or newly allocated.
 *
 * @param blkif the block interface
 */
static void *
td_xenblkif_bufcache_get(struct td_xenblkif * const blkif)
{
    void *buf;

    ASSERT(blkif);

    if (!blkif->n_reqs_bufcache_free) {
	    buf = mmap(NULL, (size_t)TD_REQ_BUFFER_SIZE,
                   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (unlikely(buf == MAP_FAILED))
            buf = NULL;
    } else
        buf = blkif->reqs_bufcache[--blkif->n_reqs_bufcache_free];

    // If we just got a request, we cancel the cache expire timer
    td_xenblkif_bufcache_evt_unreg(blkif);

    return buf;
}

static void
td_xenblkif_bufcache_put(struct td_xenblkif * const blkif, void *buf)
{
    ASSERT(blkif);

    if (unlikely(!buf))
        return;

#ifdef DEBUG
	{
		int i;

		for (i = 0; i < blkif->n_reqs_bufcache_free; i++)
			ASSERT(blkif->reqs_bufcache[i] != buf);
	}
#endif

    blkif->reqs_bufcache[blkif->n_reqs_bufcache_free++] = buf;

    /* If we're in low memory mode, prune the bufcache immediately. */
    if (tapdisk_server_mem_mode() == LOW_MEMORY_MODE) {
        td_xenblkif_bufcache_free(blkif);
    } else {
        // We only set the expire event when no requests are inflight
        if (blkif->n_reqs_free == blkif->ring_size)
            td_xenblkif_bufcache_evt_reg(blkif);
    }
}

/**
 * Puts the request back to the free list of this block interface.
 *
 * @param blkif the block interface
 * @param req the request to give back
 */
static void
tapdisk_xenblkif_free_request(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const req)
{
    int put_bufcache;

    ASSERT(blkif);
    ASSERT(req);
    ASSERT(blkif->n_reqs_free < blkif->ring_size);

    put_bufcache = req->msg.nr_segments != 0;

#ifdef DEBUG
	memset(&req->msg, BLKIF_MSG_POISON, sizeof(req->msg));
#endif

    blkif->reqs_free[blkif->ring_size - (++blkif->n_reqs_free)] = &req->msg;

	if (likely(put_bufcache))
	    td_xenblkif_bufcache_put(blkif, req->vma);
}

/**
 * Returns the size, in request descriptors, of the shared ring
 *
 * @param blkif the block interface
 * @returns the size, in request descriptors, of the shared ring
 */
static int
td_blkif_ring_size(const struct td_xenblkif * const blkif)
{
    ASSERT(blkif);

    switch (blkif->proto) {
        case BLKIF_PROTOCOL_NATIVE:
            return RING_SIZE(&blkif->rings.native);

        case BLKIF_PROTOCOL_X86_32:
            return RING_SIZE(&blkif->rings.x86_32);

        case BLKIF_PROTOCOL_X86_64:
            return RING_SIZE(&blkif->rings.x86_64);

        default:
            return -EPROTONOSUPPORT;
    }
}

/**
 * Get the response that corresponds to the specified ring index in a H/W
 * independent way.
 *
 * @returns a pointer to the response, NULL on error, sets errno
 *
 * TODO use function pointers instead of switch
 * XXX only called by xenio_blkif_put_response
 */
static inline blkif_response_t *
xenio_blkif_get_response(struct td_xenblkif* const blkif, const RING_IDX rp)
{
    blkif_back_rings_t * const rings = &blkif->rings;
    blkif_response_t * p = NULL;

    switch (blkif->proto) {
        case BLKIF_PROTOCOL_NATIVE:
            p = (blkif_response_t *) RING_GET_RESPONSE(&rings->native, rp);
            break;
        case BLKIF_PROTOCOL_X86_32:
            p = (blkif_response_t *) RING_GET_RESPONSE(&rings->x86_32, rp);
            break;
        case BLKIF_PROTOCOL_X86_64:
            p = (blkif_response_t *) RING_GET_RESPONSE(&rings->x86_64, rp);
            break;
        default:
            errno = EPROTONOSUPPORT;
			return NULL;
    }

    return p;
}

/**
 * Puts a response in the ring.
 *
 * @param blkif the VBD
 * @param req the request for which the response should be put
 * @param status the status of the response (success or an error code)
 * @param final controls whether the front-end will be notified, if necessary
 *
 * TODO @req can be NULL so the function will only notify the other end. This
 * is used in the error path of tapdisk_xenblkif_queue_requests. The point is
 * that the other will just be notified, does this make sense?
 */
static int
xenio_blkif_put_response(struct td_xenblkif * const blkif,
        struct td_xenblkif_req *req, int const status, int const final)
{
    blkif_common_back_ring_t * const ring = &blkif->rings.common;

    if (req) {
        blkif_response_t * msg = xenio_blkif_get_response(blkif,
                ring->rsp_prod_pvt);
		if (!msg)
			return -errno;

        ASSERT(status == BLKIF_RSP_EOPNOTSUPP || status == BLKIF_RSP_ERROR
                || status == BLKIF_RSP_OKAY);

        msg->id = req->msg.id;

        msg->operation = req->msg.operation;

        msg->status = status;

        ring->rsp_prod_pvt++;
    }

    if (final) {
        const RING_IDX held = ring->rsp_prod_pvt - ring->sring->rsp_prod;

        /*
         * Completion coalescing. While other requests
         * of this ring are still in flight (the caller's own request is
         * still counted as pending here), hold the push and the notification
         * until coal_count responses are ready or the coal_usecs deadline
         * fires, so the guest takes one interrupt per group instead of
         * falling into lockstep. With nothing else in flight, notify now:
         * QD1 and tail latency are untouched. (stats.kicks.out keeps
         * counting group ends; stats.coal.notifies counts the notifications
         * actually sent.)
         */
        if (blkif->coal_mode & TD_COAL_ADAPTIVE) {
            /*
             * Smoothed interval between completion groups (EWMA, 1/8), used
             * by the adaptive mode to size the hold: a few group intervals
             * are enough to gather a batch, more only adds latency.
             */
            struct timespec ts;
            long long now, dt;

            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
            dt = now - blkif->coal_last_ns;
            if (blkif->coal_last_ns && dt > 0 && dt < 100000000LL) /* ignore idle gaps > 100 ms */
                blkif->coal_ewma_ns += (dt - blkif->coal_ewma_ns) / 8;
            blkif->coal_last_ns = now;
        }

        if (blkif->coal_usecs && held && held < blkif->coal_count
                && tapdisk_xenblkif_reqs_pending(blkif) > 1
                && (!(blkif->coal_mode & TD_COAL_POLL_ONLY) || blkif->in_polling)) {
            blkif->stats.coal.held++;
            if (!blkif->coal_armed) {
                unsigned int usecs = blkif->coal_usecs;
                if (blkif->coal_mode & TD_COAL_ADAPTIVE) {
                    long long u = 4 * (blkif->coal_ewma_ns / 1000);

                    if (u < 5)
                        usecs = 5;
                    else if (u < blkif->coal_usecs)
                        usecs = (unsigned int)u;
                }
                if ((blkif->coal_mode & TD_COAL_LOAD_AWARE)
                        && tapdisk_server_system_idle_cpu() < (float)blkif->poll_idle_threshold) {
                    /* dom0 is contended (polling would be refused): hold a quarter as long */
                    usecs = usecs / 4 < 5 ? 5 : usecs / 4;
                    blkif->stats.coal.arms_short++;
                }
                blkif->coal_armed = true;
                blkif->stats.coal.deadline_us_sum += usecs;
                blkif->stats.coal.arms++;
                tapdisk_server_event_set_timeout(blkif->coal_event,
                        TV_USECS(usecs));
            }
            return 0;
        }
        return tapdisk_xenblkif_notify_now(blkif);
    }

    return 0;
}

/**
 * Pushes the produced responses to the front-end and notifies it if it asked
 * for it. Disarms a pending coalescing deadline. Must hold blkif->mutex.
 */
int
tapdisk_xenblkif_notify_now(struct td_xenblkif * const blkif)
{
    blkif_common_back_ring_t * const ring = &blkif->rings.common;
    int notify;

    ASSERT(blkif);

    if (blkif->coal_armed) {
        blkif->coal_armed = false;
        tapdisk_server_event_set_timeout(blkif->coal_event, TV_INF);
    }

    RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(ring, notify);
    blkif->stats.coal.pushes++;
    if (notify) {
        int err = xenevtchn_notify(blkif->ctx->xce_handle, blkif->port);
        blkif->stats.coal.notifies++;
        if (err < 0) {
            err = -errno;
            RING_ERR(blkif, "failed to notify event channel: %s\n",
                    strerror(-err));
            return err;
        }
    }

    return 0;
}

/**
 * Coalescing deadline: push whatever responses are held.
 */
void
tapdisk_xenblkif_cb_coalesce(event_id_t id __attribute__((unused)),
        char mode __attribute__((unused)), void *private)
{
    struct td_xenblkif *blkif = private;

    ASSERT(blkif);

    pthread_mutex_lock(&blkif->mutex);
    blkif->stats.coal.timeouts++;
    /*
     * Scheduler timeouts are periodic: disarm explicitly here, whatever
     * the state, or the callback would keep firing every coal_usecs.
     */
    blkif->coal_armed = false;
    tapdisk_server_event_set_timeout(blkif->coal_event, TV_INF);
    if (likely(!blkif->dead))
        tapdisk_xenblkif_notify_now(blkif);
    pthread_mutex_unlock(&blkif->mutex);
}


/**
 * Tells whether the request requires data to be read.
 */
static inline bool
blkif_rq_rd(blkif_request_t const * const msg)
{
	return BLKIF_OP_READ == msg->operation;
}


/**
 * Tells whether the request requires data to be written.
 */
static inline bool
blkif_rq_wr(blkif_request_t const * const msg)
{
	return BLKIF_OP_WRITE == msg->operation ||
		(BLKIF_OP_WRITE_BARRIER == msg->operation && msg->nr_segments);
}


/**
 * Tells whether the request requires data to transferred.
 */
static inline bool
blkif_rq_data(blkif_request_t const * const msg)
{
	return blkif_rq_rd(msg) || blkif_rq_wr(msg);
}


/*
 * Grant-copy batching.
 *
 * The stock backend issues one IOCTL_GNTDEV_GRANT_COPY (hence at least one
 * GNTTABOP_copy hypercall) per request: at intake for writes and at
 * completion for reads. Round 11 measured exactly 1.00 grant-copy hypercall
 * per I/O even when ~11 requests arrive per ring drain (round 11). Here:
 *
 *  - the segments of all write requests taken in one ring drain are copied
 *    from the guest with a single ioctl, before any of them is queued;
 *  - the copy-to-guest of completed reads is deferred until the completion
 *    group ends (the "final" callback of tapdisk_vbd_kick), where a single
 *    ioctl serves the whole group before the responses are published.
 *
 * Per-segment status is still checked per request, and a failed batch ioctl
 * is retried request by request so that one bad request does not fail its
 * neighbours; error reporting to the guest is unchanged.
 */

/**
 * Maps the not yet mapped persistent grants of @req in one go through
 * libxengnttab (the same path that maps the ring). Grants left unmapped
 * (errors, refs beyond the table) are grant-copied by guest_copy_prepare.
 */
static void
guest_pgnt_map(struct td_xenblkif * const blkif,
        const struct td_xenblkif_req * const req)
{
    uint32_t grefs[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    uint32_t domids[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    struct td_pgnt_chunk *chunk;
    unsigned int i, j, n = 0;
    void *addr;

    for (i = 0; i < req->msg.nr_segments; i++) {
        const grant_ref_t gref = req->msg.seg[i].gref;

        if (gref >= TD_PGNT_MAX_GREF || blkif->pgnt_map[gref])
            continue;
        for (j = 0; j < n; j++)
            if (grefs[j] == gref)
                break;
        if (j == n) {
            grefs[n] = gref;
            domids[n++] = blkif->domid;
        }
    }
    if (!n)
        return;

    addr = xengnttab_map_grant_refs(blkif->ctx->xcg_handle, n, domids, grefs,
            PROT_READ | PROT_WRITE);
    chunk = addr ? malloc(sizeof(*chunk)) : NULL;
    if (!chunk) {
        blkif->pgnt_errno = errno;
        if (addr)
            xengnttab_unmap(blkif->ctx->xcg_handle, addr, n);
        blkif->stats.pgnt.map_errors++;
        return;
    }
    chunk->addr = addr;
    chunk->count = n;
    chunk->next = blkif->pgnt_chunks;
    blkif->pgnt_chunks = chunk;
    for (i = 0; i < n; i++)
        blkif->pgnt_map[grefs[i]] = addr + ((size_t)i << PAGE_SHIFT);
    blkif->pgnt_mapped += n;
    blkif->stats.pgnt.maps++;
}

/**
 * Appends the grant-copy segments of @req to @segs, starting at index *@n,
 * and records the slice in req->gcopy_off / req->gcopy_cnt. Segments whose
 * grant is persistently mapped are moved with memcpy here instead.
 */
static void
guest_copy_prepare(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const req,
        struct gntdev_grant_copy_segment * const segs, unsigned int * const n)
{
    int i;
    const bool wr = blkif_rq_wr(&req->msg);

    ASSERT(blkif);
    ASSERT(req);
    ASSERT(segs);
    ASSERT(blkif_rq_data(&req->msg));
    ASSERT(req->msg.nr_segments > 0);
    ASSERT(req->msg.nr_segments <= BLKIF_MAX_SEGMENTS_PER_REQUEST);
    ASSERT(*n + req->msg.nr_segments <= blkif->gcopy_max_segs);

    req->gcopy_off = *n;
    req->gcopy_cnt = 0;

    if (blkif->persistent)
        guest_pgnt_map(blkif, req);

    for (i = 0; i < req->msg.nr_segments; i++) {
        const struct blkif_request_segment *blkif_seg = &req->msg.seg[i];
        struct gntdev_grant_copy_segment *gcopy_seg;
        void *virt = req->vma + (i << PAGE_SHIFT)
            + (blkif_seg->first_sect << SECTOR_SHIFT);
        const size_t off = blkif_seg->first_sect << SECTOR_SHIFT;
        const size_t len = (blkif_seg->last_sect - blkif_seg->first_sect + 1)
            << SECTOR_SHIFT;

        if (blkif->persistent) {
            void *page = blkif_seg->gref < TD_PGNT_MAX_GREF
                ? blkif->pgnt_map[blkif_seg->gref] : NULL;

            if (page) {
                if (wr)
                    memcpy(virt, page + off, len);
                else
                    memcpy(page + off, virt, len);
                blkif->stats.pgnt.segs++;
                continue;
            }
            blkif->stats.pgnt.fallback++;
        }

        gcopy_seg = &segs[*n + req->gcopy_cnt++];
        if (wr) {
            /* copy from guest */
            gcopy_seg->dest.virt = virt;
            gcopy_seg->source.foreign.ref = blkif_seg->gref;
            gcopy_seg->source.foreign.offset =
                blkif_seg->first_sect << SECTOR_SHIFT;
            gcopy_seg->source.foreign.domid = blkif->domid;
            gcopy_seg->flags = GNTCOPY_source_gref;
        } else {
            /* copy to guest */
            gcopy_seg->source.virt = virt;
            gcopy_seg->dest.foreign.ref = blkif_seg->gref;
            gcopy_seg->dest.foreign.offset =
                blkif_seg->first_sect << SECTOR_SHIFT;
            gcopy_seg->dest.foreign.domid = blkif->domid;
            gcopy_seg->flags = GNTCOPY_dest_gref;
        }

        gcopy_seg->len = len;
        gcopy_seg->status = GNTST_okay;
    }

    *n += req->gcopy_cnt;
}

/**
 * Issues one grant-copy ioctl for @n segments.
 *
 * @returns 0 on success, -errno if the ioctl itself failed (in which case no
 * per-segment status is valid)
 */
static int
guest_copy_submit(struct td_xenblkif * const blkif,
        struct gntdev_grant_copy_segment * const segs, const unsigned int n)
{
    struct ioctl_gntdev_grant_copy gcopy;

    ASSERT(blkif);
    ASSERT(blkif->ctx);

    if (!n)
        return 0;

    gcopy.count = n;
    gcopy.segments = segs;

    if (ioctl(blkif->ctx->gntdev_fd, IOCTL_GNTDEV_GRANT_COPY, &gcopy))
        return -errno;

    return 0;
}

/**
 * Checks the outcome of a batched grant copy for @req. If the batch ioctl
 * itself failed (@batch_err), the request's own segments are retried alone,
 * so that a bad request does not fail the others and the error reported is
 * the request's own.
 *
 * @returns 0 on success, -errno / -EIO on failure (logged)
 */
static int
guest_copy_check(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const req,
        struct gntdev_grant_copy_segment * const segs, const int batch_err)
{
    unsigned int i;

    ASSERT(blkif);
    ASSERT(req);
    ASSERT(req->gcopy_cnt <= req->msg.nr_segments);

    if (unlikely(batch_err)) {
        int err;

        blkif->stats.gcopy.retries++;
        err = guest_copy_submit(blkif, segs + req->gcopy_off, req->gcopy_cnt);
        if (err) {
            RING_ERR(blkif, "failed to grant-copy request %"PRIu64" "
                    "(%d segments): %s\n", req->msg.id,
                    req->msg.nr_segments, strerror(-err));
            return err;
        }
    }

    for (i = 0; i < req->gcopy_cnt; i++) {
        const struct gntdev_grant_copy_segment *gcopy_seg =
            &segs[req->gcopy_off + i];
        if (gcopy_seg->status != GNTST_okay) {
            /*
             * TODO use gnttabop_error for reporting errors, defined in
             * xen/extras/mini-os/include/gnttab.h (header not available to
             * user space)
             */
            RING_ERR(blkif, "req %lu: failed to grant-copy segment %u: %d\n",
                    req->msg.id, i, gcopy_seg->status);
            return -EIO;
        }
    }

    return 0;
}


static bool
tapdisk_xenblkif_complete_request(struct td_xenblkif * const blkif,
		struct td_xenblkif_req* req, int err, const int final,
		bool lock);

/**
 * Second half of request completion: statistics, response, release of the
 * intermediate request, ring re-check and barrier follow-up. For reads, the
 * data has already been copied to the guest (or @err says why not).
 *
 * Must be called with blkif->mutex held.
 */
static void
__tapdisk_xenblkif_complete_locked(struct td_xenblkif * const blkif,
		struct td_xenblkif_req * const req, const int err, const int final,
		const bool processing_barrier_message)
{
	int _err;
	long long *max = NULL, *sum = NULL, *cnt = NULL;
	bool blkif_destroyed = false;
	uint64_t *ticks = NULL;

	ASSERT(blkif);
	ASSERT(req);

	if (likely(!blkif->dead)) {
		if (blkif_rq_rd(&req->msg)) {
			if (likely(blkif->stats.xenvbd)) {
				cnt = &blkif->stats.xenvbd->st_rd_cnt;
				sum = &blkif->stats.xenvbd->st_rd_sum_usecs;
				max = &blkif->stats.xenvbd->st_rd_max_usecs;
			}
			blkif->vbd_stats.stats->read_reqs_completed++;
			ticks = &blkif->vbd_stats.stats->read_total_ticks;
		} else if (blkif_rq_wr(&req->msg)) {
			if (likely(blkif->stats.xenvbd)) {
				cnt = &blkif->stats.xenvbd->st_wr_cnt;
				sum = &blkif->stats.xenvbd->st_wr_sum_usecs;
				max = &blkif->stats.xenvbd->st_wr_max_usecs;
			}
			blkif->vbd_stats.stats->write_reqs_completed++;
			ticks = &blkif->vbd_stats.stats->write_total_ticks;
		}

		if (likely(cnt)) {
			struct timeval now;
			long long interval;
			gettimeofday(&now, NULL);
			interval = timeval_to_us(&now) - timeval_to_us(&req->ts);
			*ticks += interval;
			if (interval > *max)
				*max = interval;

			*sum += interval;
			*cnt += 1;
		}

		if (likely(err == 0))
			_err = BLKIF_RSP_OKAY;
		else if (err == EOPNOTSUPP)
			_err = BLKIF_RSP_EOPNOTSUPP;
		else
			_err = BLKIF_RSP_ERROR;

		xenio_blkif_put_response(blkif, req, _err, final);
	}

	tapdisk_xenblkif_free_request(blkif, req);

	blkif->stats.reqs.out++;
	if (final)
		blkif->stats.kicks.out++;

	if (unlikely(processing_barrier_message))
		blkif->barrier.msg = NULL;

	/*
	 * Schedule a ring check in case we left requests in it due to lack of
	 * memory or in case we stopped processing it because of a barrier.
	 *
	 * FIXME we should decide whether a ring check is necessary more
	 * intelligently.
	*/
	if (!blkif->barrier.msg) {
		if (likely(!blkif->dead))
			tapdisk_xenblkif_sched_chkrng(blkif);
	} else {
		/*
		 * If this is the last request, complete the barrier request.
		 */
		if (tapdisk_xenblkif_barrier_should_complete(blkif)) {
			blkif_destroyed = tapdisk_xenblkif_complete_request(blkif,
					msg_to_tapreq(blkif->barrier.msg), 0, 1, false);
			/*
			 * We assert here on "blkif_destroyed == true" because as
			 * "depth > 1" in the recursive call above, the branch to
			 * destroy the ring shouldn't be taken.
			 */
			ASSERT(!blkif_destroyed);
		}
	}
}

/**
 * Copies the data of every deferred completed read to the guest with one
 * grant-copy ioctl, then completes them; the last one carries @final.
 *
 * Must be called with blkif->mutex held and blkif->n_rd_batch > 0.
 */
static void
tapdisk_xenblkif_flush_reads(struct td_xenblkif * const blkif, const int final)
{
	const int n = blkif->n_rd_batch;
	struct td_xenblkif_req **batch, *one;
	unsigned int n_segs = 0;
	int i, batch_err;

	ASSERT(blkif);
	ASSERT(n > 0 && n <= blkif->ring_size);

	/*
	 * Take the batch off the blkif first: completing a request may
	 * re-enter tapdisk_xenblkif_complete_request (barrier follow-up).
	 * A group of one (queue depth 1) needs no copy of the array.
	 */
	if (n == 1) {
		one = blkif->rd_batch[0];
		batch = &one;
	} else {
		batch = alloca(n * sizeof(*batch));
		memcpy(batch, blkif->rd_batch, n * sizeof(*batch));
	}
	blkif->n_rd_batch = 0;

	for (i = 0; i < n; i++)
		guest_copy_prepare(blkif, batch[i], blkif->gcopy_rd_segs, &n_segs);

	batch_err = guest_copy_submit(blkif, blkif->gcopy_rd_segs, n_segs);
	if (n_segs)
		blkif->stats.gcopy.rd_ioctls++;
	blkif->stats.gcopy.rd_reqs += n;
	blkif->stats.gcopy.rd_segs += n_segs;

	for (i = 0; i < n; i++) {
		int err = guest_copy_check(blkif, batch[i], blkif->gcopy_rd_segs,
				batch_err);
		if (unlikely(err))
			RING_ERR(blkif, "req %lu: failed to copy from/to guest: %s\n",
					batch[i]->msg.id, strerror(-err));
		__tapdisk_xenblkif_complete_locked(blkif, batch[i], err,
				final && i == n - 1, false);
	}
}

/**
 * Completes a request. If this is the last pending request of a dead block
 * interface, the block interface is destroyed, the caller must not access it
 * any more.
 *
 * Successful reads are not completed immediately: their copy to the guest
 * is deferred until the last completion of the group (@final), so that one
 * grant-copy ioctl serves all reads completed together. Everything else
 * completes at once, after flushing any deferred reads if @final.
 *
 * @blkif the VBD the request belongs belongs to
 * @req the request to complete
 * @error completion status of the request
 * @final controls whether the other end should be notified
 * @lock must always be true except in this function to control recursion
 */
static bool
tapdisk_xenblkif_complete_request(struct td_xenblkif * const blkif,
		struct td_xenblkif_req* req, int err, const int final,
		bool lock)
{
	static int depth = 0;
	bool processing_barrier_message;
	bool blkif_destroyed = false;
	bool deferrable;

	ASSERT(blkif);
	ASSERT(req);
	ASSERT(depth >= 0);

	if (lock)
		pthread_mutex_lock(&blkif->mutex);
	depth++;

	deferrable = likely(!blkif->dead) && likely(!err) && blkif_rq_rd(&req->msg);

	/*
	 * This is the last completion of a group and it will not be parked
	 * itself: complete the parked reads first, whatever happens below.
	 * In particular a data-carrying barrier may leave through the early
	 * exit just after this, waiting for the ring to drain, and a parked
	 * read would then never be flushed (deadlock: the barrier waits for
	 * the read, the read waits for a final completion that cannot come
	 * because the ring is blocked behind the barrier).
	 */
	if (final && !deferrable && blkif->n_rd_batch)
		tapdisk_xenblkif_flush_reads(blkif, 0);

	processing_barrier_message =
		req->msg.operation == BLKIF_OP_WRITE_BARRIER;

	/*
	 * If a barrier request completes, check whether it's an I/O completion
	 * (the barrier carries write I/O data), or a completion because the last
	 * pending non-barrier request completed. If the former case is true, we
	 * need to check again whether the latter is true and proceed with the
	 * completion, otherwise simply note the fact that I/O is done and when
	 * the last pending non-barrier requests completes, this function will be
	 * called again passing the barrier request.
	 */
	if (unlikely(processing_barrier_message)) {
		ASSERT(blkif->barrier.msg == &req->msg);
		if (req->msg.nr_segments && !blkif->barrier.io_done) {
			blkif->barrier.io_err = err;
			blkif->barrier.io_done = true;
		}
		if (!tapdisk_xenblkif_barrier_should_complete(blkif))
			goto out;
	}

	if (deferrable) {
		/*
		 * Deferred: the request stays pending (not freed) until the flush,
		 * which keeps the barrier and dead-ring accounting unchanged.
		 * tapdisk_vbd_kick always ends a group with final == 1, and every
		 * final completion flushes (here, or above for non-reads).
		 */
		ASSERT(blkif->n_rd_batch < blkif->ring_size);
		blkif->rd_batch[blkif->n_rd_batch++] = req;
		if (!final && !blkif->gcopy_nobatch)
			goto out;
		tapdisk_xenblkif_flush_reads(blkif, final);
	} else {
		ASSERT(!final || !blkif->n_rd_batch);
		__tapdisk_xenblkif_complete_locked(blkif, req, err, final,
				processing_barrier_message);
	}

	/*
	 * Last request of a dead ring completes, destroy the ring.
	 */
	if (unlikely(1 == depth
				&& blkif->dead
				&& !tapdisk_xenblkif_reqs_pending(blkif))) {

		RING_DEBUG(blkif, "destroying dead ring\n");
		pthread_mutex_unlock(&blkif->mutex);
		tapdisk_xenblkif_destroy(blkif);
		blkif_destroyed = true;
		lock = false; /* blkif with its mutex were destroyed above so don't try to unlock it */
	}

out:
	depth--;
	if (lock)
		pthread_mutex_unlock(&blkif->mutex);
	return blkif_destroyed;
}

/**
 * Request completion callback, executed when the tapdisk has finished
 * processing the request.
 *
 * @param vreq the completed request
 * @param error status of the request
 * @param token token previously associated with this request
 * @param final controls whether the other end should be notified
 */
static inline void
__tapdisk_xenblkif_request_cb(struct td_vbd_request * const vreq,
        const int error, void * const token, const int final)
{
    struct td_xenblkif_req *req;
    struct td_xenblkif * const blkif = token;

    ASSERT(vreq);
    ASSERT(blkif);

    req = container_of(vreq, struct td_xenblkif_req, vreq);

    if (error) {
        pthread_mutex_lock(&blkif->mutex);
        if (likely(!blkif->dead)) {
            blkif->stats.errors.img++;
            blkif->vbd_stats.stats->io_errors++;
        }
        pthread_mutex_unlock(&blkif->mutex);
    }

    tapdisk_xenblkif_complete_request(blkif, req, error, final, true);
}


/**
 * Validates and vectorises a request. For writes, the grant-copy segments
 * are appended to the blkif's intake batch (blkif->gcopy_wr_segs); the copy
 * itself is issued by tapdisk_xenblkif_queue_requests for the whole drain.
 */
static inline int
tapdisk_xenblkif_parse_request_locked(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const req)
{
    td_vbd_request_t *vreq;
    int i;
    struct td_iovec *iov;
    void *page, *next, *last;
    int err = 0;
    unsigned nr_sect = 0;

    ASSERT(blkif);
    ASSERT(req);

    vreq = &req->vreq;
    ASSERT(vreq);

    req->vma = td_xenblkif_bufcache_get(blkif);
    if (unlikely(!req->vma)) {
        err = errno;
        RING_ERR(blkif, "errno %d: invalid vma\n", err);
        goto out;
    }

    for (i = 0; i < req->msg.nr_segments; i++) {
        struct blkif_request_segment *seg = &req->msg.seg[i];

        /*
         * Note that first and last may be equal, which means only one sector
         * must be transferred.
         *
         * first_sect/last_sect are guest-controlled 8-bit values, but each
         * segment addresses at most a single page. Reject any segment whose
         * sectors fall outside the page: an out-of-range last_sect would
         * produce an oversized transfer length (which also overflows the
         * uint16_t gntdev grant-copy length) and drive out-of-bounds accesses
         * to the per-request buffer.
         */
        if (seg->last_sect < seg->first_sect ||
            seg->last_sect >= (PAGE_SIZE >> SECTOR_SHIFT)) {
            RING_ERR(blkif, "req %lu: invalid sectors %d-%d\n",
                    req->msg.id, seg->first_sect, seg->last_sect);
            err = EINVAL;
            goto out;
        }
    }

    /*
     * Vectorises the request: creates the struct iovec (in req->iov) that
     * describes each segment to be transferred. Also, merges consecutive
     * segments.
     *
     * In each loop, iov points to the previous scatter/gather element in
     * order to reuse it if the current and previous segments are
     * consecutive.
     */
    iov = req->iov - 1;
    last = NULL;
    page = req->vma;

    for (i = 0; i < req->msg.nr_segments; i++) { /* for each segment */
        struct blkif_request_segment *seg = &req->msg.seg[i];
        size_t size;

        /* first_sect/last_sect are already validated, above */

        next = page + (seg->first_sect << SECTOR_SHIFT);
        size = seg->last_sect - seg->first_sect + 1;

        if (next != last) {
            iov++;
            iov->base = next;
            iov->secs = size;
        } else /* The "else" is true if first_sect is 0. */
            iov->secs += size;

        last = iov->base + (iov->secs << SECTOR_SHIFT);
        page += PAGE_SIZE;
        nr_sect += size;
    }

    vreq->iov = req->iov;
    vreq->iovcnt = iov - req->iov + 1;
    vreq->sec = req->msg.sector_number;
    req->nr_sect = nr_sect;

    if (blkif_rq_wr(&req->msg)) {
        /* copied from the guest, and accounted, by the caller */
        guest_copy_prepare(blkif, req, blkif->gcopy_wr_segs,
                &blkif->n_gcopy_wr_segs);
    } else {
		if (likely(blkif->stats.xenvbd))
			blkif->stats.xenvbd->st_rd_sect += nr_sect;
		if (likely(blkif->vbd_stats.stats))
			blkif->vbd_stats.stats->read_sectors += nr_sect;
    }

    /*
     * TODO Isn't this kind of expensive to do for each requests? Why does
     * the tapdisk need this in the first place?
     */
    snprintf(req->name, sizeof(req->name), "xenvbd-%d-%d.%"SCNx64"",
             blkif->domid, blkif->devid, req->msg.id);

    vreq->name = req->name;
    vreq->token = blkif;
    vreq->cb = __tapdisk_xenblkif_request_cb;

out:
    return err;
}


/**
 * Initialises the standard tapdisk request (td_vbd_request_t) from the
 * intermediate ring request (td_xenblkif_req) in order to prepare it
 * processing.
 *
 * @param blkif the block interface
 * @param req the request to prepare
 * @returns 0 on success
 *
 * XXX only called by tapdisk_xenblkif_queue_requests
 */
static inline int
tapdisk_xenblkif_make_vbd_request(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const req)
{
    int err = 0;
    td_vbd_request_t *vreq;
    bool blkif_freed = false;

    ASSERT(req);

    vreq = &req->vreq;
    ASSERT(vreq);
    memset(vreq, 0, sizeof(*vreq));

    req->vma = NULL;
    switch (req->msg.operation) {
    case BLKIF_OP_READ:
        if (likely(blkif->stats.xenvbd))
			blkif->stats.xenvbd->st_rd_req++;
	if (likely(blkif->vbd_stats.stats))
		blkif->vbd_stats.stats->read_reqs_submitted++;
        req->prot = PROT_WRITE;
        vreq->op = TD_OP_READ;
        break;
    case BLKIF_OP_WRITE:
    case BLKIF_OP_WRITE_BARRIER:
        if (likely(blkif->stats.xenvbd))
			blkif->stats.xenvbd->st_wr_req++;
	if (likely(blkif->vbd_stats.stats))
		blkif->vbd_stats.stats->write_reqs_submitted++;
        req->prot = PROT_READ;
        vreq->op = TD_OP_WRITE;
        break;
    default:
        RING_ERR(blkif, "req %lu: invalid request type %d\n",
                req->msg.id, req->msg.operation);
        err = EOPNOTSUPP;
        goto out;
    }
    /* Timestamp before the requests leave the blkif layer */
    gettimeofday(&req->ts, NULL);

    /*
     * Check that the number of segments is sane. nr_segments is guest-
     * controlled; the blkif protocol permits at most
     * BLKIF_MAX_SEGMENTS_PER_REQUEST segments per request, which is how the
     * ring descriptor and our per-request buffers are sized.
     */
    if (unlikely((req->msg.nr_segments == 0 &&
                req->msg.operation != BLKIF_OP_WRITE_BARRIER) ||
            req->msg.nr_segments > BLKIF_MAX_SEGMENTS_PER_REQUEST)) {
        RING_ERR(blkif, "req %lu: bad number of segments in request (%d)\n",
                req->msg.id, req->msg.nr_segments);
        err = EINVAL;
        goto out;
    }

    if (likely(req->msg.nr_segments)) {
        pthread_mutex_lock(&blkif->mutex);
        err = tapdisk_xenblkif_parse_request_locked(blkif, req);
        pthread_mutex_unlock(&blkif->mutex);
    /*
     * If we only got one request from the ring and that was a barrier one,
     * check whether the barrier requests completion conditions are satisfied
	 * and if they are, complete the barrier request.
     *
     * It could be that there are more requests in the ring after the barrier
     * request, tapdisk_xenblkif_complete_request() will schedule a ring check.
     */
    } else {
        pthread_mutex_lock(&blkif->mutex);
        if (tapdisk_xenblkif_barrier_should_complete(blkif)) {
            blkif_freed = tapdisk_xenblkif_complete_request(blkif,
                    msg_to_tapreq(blkif->barrier.msg), 0, 1, false);
            err = 0;
        }
        if (!blkif_freed)
            pthread_mutex_unlock(&blkif->mutex);
    }
out:
    return err;
}


/**
 * Queues the requests of one ring drain to the standard tapdisk queue.
 *
 * Pass 1 validates and vectorises every request and collects the segments
 * of all writes; pass 2 copies the write data from the guest with one
 * grant-copy ioctl; pass 3 queues the requests in ring order (or fails the
 * ones whose validation or copy failed).
 *
 * The intake batch (blkif->gcopy_wr_segs) is private to this function, which
 * only runs from the ring-processing path on the scheduler thread.
 */
void
tapdisk_xenblkif_queue_requests(struct td_xenblkif * const blkif,
        blkif_request_t *reqs[], const int nr_reqs)
{
    int i;
    int nr_errors = 0;
    int nr_wr = 0;
    int batch_err = 0;
    bool blkif_freed = false;
    int *errs;
    bool *skip;

    ASSERT(blkif);
    ASSERT(reqs);
    ASSERT(nr_reqs >= 0);

    if (!nr_reqs)
        return;

    errs = alloca(nr_reqs * sizeof(*errs));
    skip = alloca(nr_reqs * sizeof(*skip));

    blkif->n_gcopy_wr_segs = 0;

    for (i = 0; i < nr_reqs; i++) { /* for each request in the ring... */
        struct td_xenblkif_req *req = msg_to_tapreq(reqs[i]);
        const bool has_data = req->msg.nr_segments != 0;
        const bool wr = blkif_rq_wr(&req->msg);

        ASSERT(req);

        /*
         * Do not use req after tapdisk_xenblkif_make_vbd_request when it
         * carries no data: a bare barrier may be completed there, its msg
         * reinserted in the reqs_free array, and (if the ring is dead and
         * it was the last pending request) the blkif destroyed. A bare
         * barrier is always the last request of a drain, and it can only
         * complete there if it is the sole request of the drain, since the
         * other requests of the drain are still pending.
         */
        errs[i] = tapdisk_xenblkif_make_vbd_request(blkif, req);
        skip[i] = !has_data && !errs[i];
        if (!errs[i] && has_data && wr)
            nr_wr++;
    }

    if (nr_wr && !blkif->gcopy_nobatch) {
        batch_err = guest_copy_submit(blkif, blkif->gcopy_wr_segs,
                blkif->n_gcopy_wr_segs);
        if (blkif->n_gcopy_wr_segs)
            blkif->stats.gcopy.wr_ioctls++;
        blkif->stats.gcopy.wr_reqs += nr_wr;
        blkif->stats.gcopy.wr_segs += blkif->n_gcopy_wr_segs;
    }

    for (i = 0; i < nr_reqs && !blkif_freed; i++) {
        struct td_xenblkif_req *req;
        int err = errs[i];

        if (skip[i])
            continue;

        req = msg_to_tapreq(reqs[i]);

        if (likely(!err) && blkif_rq_wr(&req->msg)) {
            if (blkif->gcopy_nobatch && req->gcopy_cnt) {
                batch_err = guest_copy_submit(blkif,
                        blkif->gcopy_wr_segs + req->gcopy_off, req->gcopy_cnt);
                blkif->stats.gcopy.wr_ioctls++;
                blkif->stats.gcopy.wr_reqs++;
                blkif->stats.gcopy.wr_segs += req->gcopy_cnt;
            }
            err = guest_copy_check(blkif, req, blkif->gcopy_wr_segs,
                    batch_err);
            if (unlikely(err)) {
                RING_ERR(blkif, "req %lu: failed to copy from guest: %s\n",
                        req->msg.id, strerror(-err));
            } else {
                pthread_mutex_lock(&blkif->mutex);
                if (likely(blkif->stats.xenvbd))
                    blkif->stats.xenvbd->st_wr_sect += req->nr_sect;
                if (likely(blkif->vbd_stats.stats))
                    blkif->vbd_stats.stats->write_sectors += req->nr_sect;
                pthread_mutex_unlock(&blkif->mutex);
            }
        }

        if (unlikely(err)) {
            /* TODO log error */
            blkif->stats.errors.map++;
        } else {
            err = tapdisk_vbd_queue_request(blkif->vbd, &req->vreq);
            if (unlikely(err)) {
                /* TODO log error */
                blkif->stats.errors.vbd++;
            }
        }

        if (unlikely(err)) {
            nr_errors++;
            blkif_freed = tapdisk_xenblkif_complete_request(blkif, req, err,
                    1, true);
        }
    }

    /* there is a possibility of blkif getting freed if ring is
       dead and current request is the last one, hence adding
       this check to avoid seg fault */

    if (nr_errors && !blkif_freed) {
        pthread_mutex_lock(&blkif->mutex);
        xenio_blkif_put_response(blkif, NULL, 0, 1);
        pthread_mutex_unlock(&blkif->mutex);
    }
}

void
tapdisk_xenblkif_reqs_free(struct td_xenblkif * const blkif)
{
    ASSERT(blkif);

    td_xenblkif_bufcache_free(blkif);
    td_xenblkif_bufcache_evt_unreg(blkif);

    free(blkif->reqs_bufcache);
    blkif->reqs_bufcache = NULL;

    free(blkif->gcopy_wr_segs);
    blkif->gcopy_wr_segs = NULL;
    free(blkif->gcopy_rd_segs);
    blkif->gcopy_rd_segs = NULL;
    free(blkif->rd_batch);
    blkif->rd_batch = NULL;

    free(blkif->reqs);
    blkif->reqs = NULL;

    free(blkif->reqs_free);
    blkif->reqs_free = NULL;

    pthread_mutex_destroy(&blkif->mutex);
}

int
tapdisk_xenblkif_reqs_init(struct td_xenblkif *td_blkif)
{
    void *buf;
    int i = 0;
    int err = 0;

    ASSERT(td_blkif);

    pthread_mutex_init(&td_blkif->mutex, NULL);

    td_blkif->ring_size = td_blkif_ring_size(td_blkif);
    ASSERT(td_blkif->ring_size > 0);

    td_blkif->reqs =
        calloc(td_blkif->ring_size, sizeof(struct td_xenblkif_req));
    if (!td_blkif->reqs) {
        err = -errno;
        goto fail;
    }

    td_blkif->reqs_free =
        malloc(td_blkif->ring_size * sizeof(struct blkif_request_t *));
    if (!td_blkif->reqs_free) {
        err = -errno;
        goto fail;
    }

    /* Grant-copy batches: every request of the ring, fully segmented. */
    td_blkif->gcopy_max_segs =
        td_blkif->ring_size * BLKIF_MAX_SEGMENTS_PER_REQUEST;
    td_blkif->gcopy_wr_segs = calloc(td_blkif->gcopy_max_segs,
            sizeof(*td_blkif->gcopy_wr_segs));
    td_blkif->gcopy_rd_segs = calloc(td_blkif->gcopy_max_segs,
            sizeof(*td_blkif->gcopy_rd_segs));
    td_blkif->rd_batch = calloc(td_blkif->ring_size,
            sizeof(*td_blkif->rd_batch));
    if (!td_blkif->gcopy_wr_segs || !td_blkif->gcopy_rd_segs
            || !td_blkif->rd_batch) {
        err = -errno;
        goto fail;
    }
    td_blkif->n_gcopy_wr_segs = 0;
    td_blkif->n_rd_batch = 0;
    {
        /* /etc/tapdisk-gcopy.conf starting with 0: batching off */
        FILE *f = fopen("/etc/tapdisk-gcopy.conf", "r");
        int c = f ? fgetc(f) : EOF;

        if (f)
            fclose(f);
        td_blkif->gcopy_nobatch = (c == '0');
        if (td_blkif->gcopy_nobatch)
            RING_DEBUG(td_blkif, "grant-copy batching off (kill-switch)\n");
    }

    td_blkif->n_reqs_free = 0;
    for (i = 0; i < td_blkif->ring_size; i++)
        tapdisk_xenblkif_free_request(td_blkif, &td_blkif->reqs[i]);

    // Allocate the buffer cache
    td_blkif->reqs_bufcache = malloc(sizeof(void*) * td_blkif->ring_size);
    if (!td_blkif->reqs_bufcache) {
        err = -errno;
        goto fail;
    }
    td_blkif->n_reqs_bufcache_free = 0;
    td_blkif->reqs_bufcache_evtid = 0;

    // Populate cache with one buffer
    buf = td_xenblkif_bufcache_get(td_blkif);
    td_xenblkif_bufcache_put(td_blkif, buf);
    td_xenblkif_bufcache_evt_unreg(td_blkif);

    return 0;

fail:
    tapdisk_xenblkif_reqs_free(td_blkif);
    return err;
}
