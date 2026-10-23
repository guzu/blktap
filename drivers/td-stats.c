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

#include "debug.h"
#include "tapdisk-log.h"
#include "td-stats.h"
#include "td-ctx.h"

void
tapdisk_xenblkif_stats(struct td_xenblkif * blkif, td_stats_t * st)
{
    ASSERT(blkif);
    ASSERT(st);
    ASSERT(blkif->ctx);

    tapdisk_stats_field(st, "pool", "s", blkif->ctx->pool_name);
    tapdisk_stats_field(st, "domid", "d", blkif->domid);
    tapdisk_stats_field(st, "devid", "d", blkif->devid);

    tapdisk_stats_field(st, "reqs", "[");
    tapdisk_stats_val(st, "llu", blkif->stats.reqs.in);
    tapdisk_stats_val(st, "llu", blkif->stats.reqs.out);
    tapdisk_stats_leave(st, ']');

    tapdisk_stats_field(st, "kicks", "[");
    tapdisk_stats_val(st, "llu", blkif->stats.kicks.in);
    tapdisk_stats_val(st, "llu", blkif->stats.kicks.out);
    tapdisk_stats_leave(st, ']');

    tapdisk_stats_field(st, "errors", "{");
    tapdisk_stats_field(st, "msg", "llu", blkif->stats.errors.msg);
    tapdisk_stats_field(st, "map", "llu", blkif->stats.errors.map);
    tapdisk_stats_field(st, "vbd", "llu", blkif->stats.errors.vbd);
    tapdisk_stats_field(st, "img", "llu", blkif->stats.errors.img);
    tapdisk_stats_leave(st, '}');

    tapdisk_stats_field(st, "pgnt", "{");
    tapdisk_stats_field(st, "enabled", "d", blkif->persistent ? 1 : 0);
    tapdisk_stats_field(st, "mapped", "u", blkif->pgnt_mapped);
    tapdisk_stats_field(st, "maps", "llu", blkif->stats.pgnt.maps);
    tapdisk_stats_field(st, "map_errors", "llu", blkif->stats.pgnt.map_errors);
    tapdisk_stats_field(st, "segs", "llu", blkif->stats.pgnt.segs);
    tapdisk_stats_field(st, "fallback", "llu", blkif->stats.pgnt.fallback);
    tapdisk_stats_field(st, "errno", "d", blkif->pgnt_errno);
    tapdisk_stats_leave(st, '}');

    tapdisk_stats_field(st, "poll", "{");
    tapdisk_stats_field(st, "max_us", "u", blkif->apoll_max);
    tapdisk_stats_field(st, "min_us", "u", blkif->apoll_min);
    tapdisk_stats_field(st, "cur_us", "u", blkif->apoll_cur);
    tapdisk_stats_field(st, "duration_us", "d", blkif->poll_duration);
    tapdisk_stats_field(st, "starts", "llu", blkif->stats.poll.starts);
    tapdisk_stats_field(st, "denied", "llu", blkif->stats.poll.denied);
    tapdisk_stats_field(st, "expired", "llu", blkif->stats.poll.expired);
    tapdisk_stats_field(st, "rearmed", "llu", blkif->stats.poll.rearmed);
    tapdisk_stats_field(st, "mode", "u", blkif->apoll_mode);
    tapdisk_stats_field(st, "wakeups", "llu", blkif->stats.poll.wakeups);
    tapdisk_stats_field(st, "grows", "llu", blkif->stats.poll.grows);
    tapdisk_stats_field(st, "shrinks", "llu", blkif->stats.poll.shrinks);
    tapdisk_stats_field(st, "block_ns_sum", "llu", blkif->stats.poll.block_ns_sum);
    tapdisk_stats_field(st, "polled_ns", "llu", blkif->stats.poll.polled_ns);
    tapdisk_stats_field(st, "window_us_sum", "llu", blkif->stats.poll.window_us_sum);
    tapdisk_stats_leave(st, '}');

    tapdisk_stats_field(st, "coal", "{");
    tapdisk_stats_field(st, "usecs", "u", blkif->coal_usecs);
    tapdisk_stats_field(st, "count", "u", blkif->coal_count);
    tapdisk_stats_field(st, "pushes", "llu", blkif->stats.coal.pushes);
    tapdisk_stats_field(st, "notifies", "llu", blkif->stats.coal.notifies);
    tapdisk_stats_field(st, "held", "llu", blkif->stats.coal.held);
    tapdisk_stats_field(st, "timeouts", "llu", blkif->stats.coal.timeouts);
    tapdisk_stats_field(st, "mode", "u", blkif->coal_mode);
    tapdisk_stats_field(st, "arms", "llu", blkif->stats.coal.arms);
    tapdisk_stats_field(st, "deadline_us_sum", "llu", blkif->stats.coal.deadline_us_sum);
    tapdisk_stats_field(st, "ewma_ns", "lld", blkif->coal_ewma_ns);
    tapdisk_stats_field(st, "arms_short", "llu", blkif->stats.coal.arms_short);
    tapdisk_stats_leave(st, '}');

    tapdisk_stats_field(st, "gcopy", "{");
    tapdisk_stats_field(st, "wr_ioctls", "llu", blkif->stats.gcopy.wr_ioctls);
    tapdisk_stats_field(st, "wr_reqs", "llu", blkif->stats.gcopy.wr_reqs);
    tapdisk_stats_field(st, "wr_segs", "llu", blkif->stats.gcopy.wr_segs);
    tapdisk_stats_field(st, "rd_ioctls", "llu", blkif->stats.gcopy.rd_ioctls);
    tapdisk_stats_field(st, "rd_reqs", "llu", blkif->stats.gcopy.rd_reqs);
    tapdisk_stats_field(st, "rd_segs", "llu", blkif->stats.gcopy.rd_segs);
    tapdisk_stats_field(st, "retries", "llu", blkif->stats.gcopy.retries);
    tapdisk_stats_leave(st, '}');
}
