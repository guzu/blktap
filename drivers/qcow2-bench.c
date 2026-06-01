/*
 * qcow2-bench: benchmark tool reproducing block-qcow2.c's multiqueue dispatch
 * model.
 *
 * Like tapdisk in multiqueue mode, N producer pthreads each service their own
 * "ring" (queue): a producer fills request slots from a per-queue free list,
 * enqueues them under the queue lock and kicks the queue's BH cross-thread
 * (qemu_bh_schedule -> aio_notify). The BH runs on that queue's IOThread
 * AioContext (the consumer), drains the inflight list and submits the
 * blk_aio_pre{ad,wr}itev from its own ctx. Completions return the slot to the
 * free list and wake the producer. A single BlockBackend is shared by all
 * queues and attached to queue[0]'s AioContext (block-qcow2.c:607).
 *
 * This mirrors drivers/block-qcow2.c: schedule_request() (producer side),
 * qcow2_handle_requests()/block_bh (consumer side) and signal_completion().
 */

#include "qemu/osdep.h"

#include <getopt.h>
#include <sys/time.h>

#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qapi-commands-qom.h"
#include "qapi/qapi-types-qom.h"

#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/atomic.h"
#include "qemu/cutils.h"
#include "qemu/memalign.h"
#include "qemu/error-report.h"
#include "qemu/qcow2-counters.h"
#include "qemu/queue.h"
#include "qemu/thread.h"
#include "qemu/defer-call.h"
#include "qom/object_interfaces.h"

#include "block/aio.h"
#include "block/block.h"
#include "sysemu/block-backend.h"
#include "sysemu/iothread.h"

#define POLL_MAX_NS_UNSET (-1)
#define IO_PLUG_THRESHOLD 1

enum bench_op {
    BENCH_OP_READ,
    BENCH_OP_WRITE,
    BENCH_OP_FLUSH,
};

struct bench_shared {
    int total_remaining;   /* atomic: data requests not yet completed (all queues) */
    AioContext *main_ctx;
};

struct bench_req {
    QEMUIOVector              qiov;       /* points at this slot's buffer slice */
    int64_t                   offset;
    enum bench_op             op;
    struct bench_queue       *q;
    QSIMPLEQ_ENTRY(bench_req) entry;
};

struct bench_queue {
    /* shared/config (read-only after setup) */
    BlockBackend             *blk;
    uint64_t                  image_size;
    bool                      write;
    int                       bufsize;
    int                       step;          /* per-queue offset stride */
    int                       depth;         /* free-list size = max in-flight */
    int                       flush_interval;
    bool                      drain_on_flush;
    struct bench_shared      *shared;
    int                       id;

    /* per-queue IOThread plumbing (consumer side) */
    IOThread                 *iothread;
    char                     *iothread_id;
    AioContext               *ctx;
    QEMUBH                   *bh;
    MemReentrancyGuard        reentrancy_guard;

    /* ring state, guarded by lock; shared between producer thread, the BH
     * (iothread ctx) and the completion callback (BB ctx). */
    QemuMutex                 lock;
    QemuCond                  slot_avail;    /* a slot returned to the free list */
    struct bench_req         *reqs;          /* depth slots */
    struct bench_req        **freelist;      /* depth free pointers */
    int                       free_count;
    int                       n_queued;      /* items waiting in inflight */
    QSIMPLEQ_HEAD(, bench_req) inflight;
    uint8_t                  *buf;           /* depth * bufsize, aligned */

    /* producer-only state */
    QemuThread                producer;
    int                       n;             /* data requests to produce */
    uint64_t                  offset;        /* running offset */
};

static void bench_complete(void *opaque, int ret);

/* mirrors qcow2_handle_requests()/block_bh: runs on the IOThread ctx. */
static void bench_bh(void *opaque)
{
    struct bench_queue *q = opaque;
    struct bench_req *req;
    int inflight_atstart;
    int batched = 0;

    qemu_mutex_lock(&q->lock);
    inflight_atstart = q->n_queued;
    if (inflight_atstart > IO_PLUG_THRESHOLD) {
        defer_call_begin();
    }
    while ((req = QSIMPLEQ_FIRST(&q->inflight))) {
        QSIMPLEQ_REMOVE_HEAD(&q->inflight, entry);
        q->n_queued--;
        qemu_mutex_unlock(&q->lock);

        if (inflight_atstart > IO_PLUG_THRESHOLD &&
            batched >= inflight_atstart) {
            defer_call_end();
        }

        switch (req->op) {
        case BENCH_OP_READ:
            blk_aio_preadv(q->blk, req->offset, &req->qiov, 0,
                           bench_complete, req);
            break;
        case BENCH_OP_WRITE:
            blk_aio_pwritev(q->blk, req->offset, &req->qiov, 0,
                            bench_complete, req);
            break;
        case BENCH_OP_FLUSH:
            blk_aio_flush(q->blk, bench_complete, req);
            break;
        }

        if (inflight_atstart > IO_PLUG_THRESHOLD) {
            if (batched >= inflight_atstart) {
                defer_call_begin();
                batched = 0;
            } else {
                batched++;
            }
        }
        qemu_mutex_lock(&q->lock);
    }
    qemu_mutex_unlock(&q->lock);

    if (inflight_atstart > IO_PLUG_THRESHOLD) {
        defer_call_end();
    }
}

/* mirrors signal_completion()/free_qcow2_request: runs on the BB ctx. */
static void bench_complete(void *opaque, int ret)
{
    struct bench_req *req = opaque;
    struct bench_queue *q = req->q;
    bool is_data = (req->op != BENCH_OP_FLUSH);

    if (ret < 0) {
        error_report("queue %d: request failed: %s", q->id, strerror(-ret));
        exit(EXIT_FAILURE);
    }

    qemu_mutex_lock(&q->lock);
    q->freelist[q->free_count++] = req;
    qemu_cond_signal(&q->slot_avail);
    qemu_mutex_unlock(&q->lock);

    if (is_data) {
        if (qatomic_dec_fetch(&q->shared->total_remaining) == 0) {
            aio_notify(q->shared->main_ctx);
        }
    }
}

/* grab a free slot, blocking until one is available */
static struct bench_req *bench_get_slot(struct bench_queue *q)
{
    struct bench_req *req;

    qemu_mutex_lock(&q->lock);
    while (q->free_count == 0) {
        qemu_cond_wait(&q->slot_avail, &q->lock);
    }
    req = q->freelist[--q->free_count];
    qemu_mutex_unlock(&q->lock);
    return req;
}

static void bench_submit(struct bench_queue *q, struct bench_req *req)
{
    qemu_mutex_lock(&q->lock);
    QSIMPLEQ_INSERT_TAIL(&q->inflight, req, entry);
    q->n_queued++;
    qemu_mutex_unlock(&q->lock);
    qemu_bh_schedule(q->bh);
}

/* one producer pthread per queue: mirrors the tapdisk ring-servicing thread
 * that funnels guest requests through schedule_request(). */
static void *bench_producer(void *opaque)
{
    struct bench_queue *q = opaque;
    int produced = 0;
    int since_flush = 0;

    while (produced < q->n) {
        struct bench_req *req = bench_get_slot(q);

        req->op = q->write ? BENCH_OP_WRITE : BENCH_OP_READ;
        req->offset = q->offset;
        q->offset += q->step;
        if (q->image_size <= (uint64_t)q->bufsize) {
            q->offset = 0;
        } else {
            q->offset %= q->image_size - q->bufsize;
        }

        bench_submit(q, req);
        produced++;
        since_flush++;

        if (q->flush_interval && since_flush >= q->flush_interval) {
            struct bench_req *freq;
            since_flush = 0;

            if (q->drain_on_flush) {
                /* wait for this queue's in-flight requests to quiesce */
                qemu_mutex_lock(&q->lock);
                while (q->free_count < q->depth) {
                    qemu_cond_wait(&q->slot_avail, &q->lock);
                }
                qemu_mutex_unlock(&q->lock);
            }

            freq = bench_get_slot(q);
            freq->op = BENCH_OP_FLUSH;
            bench_submit(q, freq);
        }
    }

    return NULL;
}

/* mirrors qcow2_iothread_create() in drivers/block-qcow2.c */
static IOThread *bench_iothread_create(const char *id, Error **errp)
{
    ObjectOptions *opts = g_new0(ObjectOptions, 1);
    *opts = (ObjectOptions) {
        .qom_type = OBJECT_TYPE_IOTHREAD,
        .id = g_strdup(id),
    };
    qmp_object_add(opts, errp);
    qapi_free_ObjectOptions(opts);
    if (*errp) {
        return NULL;
    }
    return iothread_by_id(id);
}

static void usage(const char *prog)
{
    fprintf(stderr,
"Usage: %s [options] FILE\n"
"\n"
"  -c COUNT          total number of I/O requests (default: 75000)\n"
"  -d DEPTH          per-queue ring depth / max in-flight (default: 64)\n"
"  -j QUEUES         number of queues (default: 1). Each queue gets its own\n"
"                    producer pthread and, when >= 1, its own IOThread,\n"
"                    matching drivers/block-qcow2.c's multiqueue model; 0\n"
"                    runs a single producer against the main loop AioContext\n"
"                    (no IOThread).\n"
"  -s BUFSIZE        request size in bytes (default: 4096)\n"
"  -S STEPSIZE       per-queue offset stride (default: BUFSIZE * QUEUES)\n"
"  -o OFFSET         starting offset (default: 0)\n"
"  -w                write test (default: read)\n"
"  --pattern BYTE    pattern byte to write (default: 0)\n"
"  --flush-interval N   issue flush every N writes per queue\n"
"  --no-drain        don't drain the queue before flush\n"
"  -t CACHE          cache mode (default: none)\n"
"  -i AIO            aio backend (threads, native, io_uring)\n"
"  -f FMT            image format (default: qcow2)\n"
"  --poll-max-ns N   AioContext adaptive poll window in ns (default:\n"
"                    upstream QEMU's IOTHREAD_POLL_MAX_NS_DEFAULT = 32768).\n"
"                    0 disables polling entirely (always blocks on ppoll).\n"
"  --co-mutex-spin N CoMutex adaptive spin limit (cpu_relax iterations a\n"
"                    waiter busy-spins before yielding; default 1000). Sets\n"
"                    QEMU_CO_MUTEX_SPIN. Higher values trade CPU for fewer\n"
"                    yield/wakeup round trips under cross-iothread contention.\n"
"  -q                quiet\n"
"  -h                this help\n",
        prog);
}

enum {
    OPT_PATTERN = 256,
    OPT_FLUSH_INTERVAL,
    OPT_NO_DRAIN,
    OPT_POLL_MAX_NS,
    OPT_CO_MUTEX_SPIN,
};

int main(int argc, char **argv)
{
    const char *fmt = "qcow2";
    const char *filename;
    const char *cache = "none";
    const char *aio_backend = NULL;
    int count = 75000;
    int depth = 64;
    int queues = 1;
    int bufsize = 4096;
    int step = 0;
    int64_t offset = 0;
    bool is_write = false;
    int pattern = 0;
    int flush_interval = 0;
    bool drain_on_flush = true;
    int64_t poll_max_ns = POLL_MAX_NS_UNSET;
    int co_mutex_spin = -1;
    bool quiet = false;
    int flags = 0;
    bool writethrough = false;
    Error *local_err = NULL;
    int ret = 0;
    int c;

    static const struct option long_opts[] = {
        {"help",           no_argument,       0, 'h'},
        {"count",          required_argument, 0, 'c'},
        {"depth",          required_argument, 0, 'd'},
        {"queues",         required_argument, 0, 'j'},
        {"buffer-size",    required_argument, 0, 's'},
        {"step-size",      required_argument, 0, 'S'},
        {"offset",         required_argument, 0, 'o'},
        {"write",          no_argument,       0, 'w'},
        {"cache",          required_argument, 0, 't'},
        {"aio",            required_argument, 0, 'i'},
        {"format",         required_argument, 0, 'f'},
        {"quiet",          no_argument,       0, 'q'},
        {"pattern",        required_argument, 0, OPT_PATTERN},
        {"flush-interval", required_argument, 0, OPT_FLUSH_INTERVAL},
        {"no-drain",       no_argument,       0, OPT_NO_DRAIN},
        {"poll-max-ns",    required_argument, 0, OPT_POLL_MAX_NS},
        {"co-mutex-spin",  required_argument, 0, OPT_CO_MUTEX_SPIN},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "hc:d:j:s:S:o:wt:i:f:q",
                            long_opts, NULL)) != -1) {
        switch (c) {
        case 'h': usage(argv[0]); return 0;
        case 'c': count = atoi(optarg); break;
        case 'd': depth = atoi(optarg); break;
        case 'j': queues = atoi(optarg); break;
        case 's': bufsize = atoi(optarg); break;
        case 'S': step = atoi(optarg); break;
        case 'o': offset = strtoll(optarg, NULL, 0); break;
        case 'w': is_write = true; flags |= BDRV_O_RDWR; break;
        case 't': cache = optarg; break;
        case 'i': aio_backend = optarg; break;
        case 'f': fmt = optarg; break;
        case 'q': quiet = true; break;
        case OPT_PATTERN: pattern = atoi(optarg) & 0xff; break;
        case OPT_FLUSH_INTERVAL: flush_interval = atoi(optarg); break;
        case OPT_NO_DRAIN: drain_on_flush = false; break;
        case OPT_POLL_MAX_NS: poll_max_ns = strtoll(optarg, NULL, 0); break;
        case OPT_CO_MUTEX_SPIN: co_mutex_spin = atoi(optarg); break;
        default: usage(argv[0]); return 1;
        }
    }
    if (optind != argc - 1) {
        usage(argv[0]);
        return 1;
    }
    filename = argv[argc - 1];

    if (count <= 0 || depth <= 0 || bufsize <= 0 || queues < 0) {
        error_report("invalid numeric argument");
        return 1;
    }
    if (!is_write && flush_interval) {
        error_report("--flush-interval is only valid with -w");
        return 1;
    }
    if (flush_interval && flush_interval < depth) {
        error_report("flush interval must be >= depth");
        return 1;
    }
    if (step == 0) {
        step = bufsize * (queues > 0 ? queues : 1);
    }

    /* Override the CoMutex spin limit before any coroutine lock is taken;
     * util/qemu-coroutine-lock.c reads QEMU_CO_MUTEX_SPIN lazily on first use. */
    if (co_mutex_spin >= 0) {
        char spinbuf[16];
        snprintf(spinbuf, sizeof(spinbuf), "%d", co_mutex_spin);
        setenv("QEMU_CO_MUTEX_SPIN", spinbuf, 1);
    }

    /* QEMU runtime init (mirrors qcow2_initialize() in block-qcow2.c) */
    qemu_thread_naming(true);
    qemu_init_cpu_loop();
    bql_lock();
    module_call_init(MODULE_INIT_QOM);
    bdrv_init();
    if (qemu_init_main_loop(&local_err) < 0) {
        error_report_err(local_err);
        return 1;
    }

    /* Parse cache mode */
    if (bdrv_parse_cache_mode(cache, &flags, &writethrough) < 0) {
        error_report("invalid cache mode: %s", cache);
        return 1;
    }

    /* Build options dict (driver=fmt, file.filename=..., file.aio=...) */
    QDict *opts = qdict_new();
    QDict *file_layer = qdict_new();
    qdict_put_str(file_layer, "filename", filename);
    if (aio_backend) {
        qdict_put_str(file_layer, "aio", aio_backend);
        if (g_str_equal(aio_backend, "native")) {
            flags |= BDRV_O_NATIVE_AIO;
        }
    }
    qdict_put_str(opts, "driver", fmt);
    qdict_put(opts, "file", file_layer);

    if (!quiet) {
        fprintf(stderr, "qcow2-bench: opening %s (fmt=%s flags=0x%x cache=%s aio=%s)\n",
                filename, fmt, flags, cache, aio_backend ? aio_backend : "default");
    }
    BlockBackend *blk = blk_new_open(filename, NULL, opts, flags, &local_err);
    if (!blk) {
        fprintf(stderr, "qcow2-bench: blk_new_open failed: %s\n",
                local_err ? error_get_pretty(local_err) : "(no error info)");
        if (flags & BDRV_O_NOCACHE) {
            fprintf(stderr, "qcow2-bench: hint: -t none uses O_DIRECT which some "
                    "filesystems reject with EINVAL (encrypted home, older tmpfs, "
                    "overlayfs, ...); try '-t writeback' or move the image to a "
                    "plain ext4/xfs path\n");
        }
        error_report_err(local_err);
        return 1;
    }
    blk_set_enable_write_cache(blk, !writethrough);

    int64_t image_size = blk_getlength(blk);
    if (image_size < 0) {
        error_report("blk_getlength failed: %s", strerror(-image_size));
        return 1;
    }

    /* Create N queues (each with its own IOThread, or the main ctx if j==0) */
    int n_queues = queues > 0 ? queues : 1;
    struct bench_queue *qs = g_new0(struct bench_queue, n_queues);
    struct bench_shared shared = {
        .total_remaining = count,
        .main_ctx        = qemu_get_aio_context(),
    };

    for (int i = 0; i < n_queues; i++) {
        struct bench_queue *q = &qs[i];
        q->id          = i;
        q->blk         = blk;
        q->image_size  = image_size;
        q->write       = is_write;
        q->bufsize     = bufsize;
        q->step        = step;
        q->depth       = depth;
        q->flush_interval = flush_interval;
        q->drain_on_flush = drain_on_flush;
        q->shared      = &shared;

        qemu_mutex_init(&q->lock);
        qemu_cond_init(&q->slot_avail);
        QSIMPLEQ_INIT(&q->inflight);

        /* shard count across queues */
        q->n = count / n_queues + (i < (count % n_queues) ? 1 : 0);
        q->offset = (uint64_t)offset + (uint64_t)i * (uint64_t)bufsize;
        if (image_size > bufsize) {
            q->offset %= image_size - bufsize;
        } else {
            q->offset = 0;
        }

        if (queues >= 1) {
            char idbuf[32];
            snprintf(idbuf, sizeof(idbuf), "bench-iothread%d", i);
            q->iothread_id = g_strdup(idbuf);
            q->iothread = bench_iothread_create(idbuf, &local_err);
            if (!q->iothread) {
                error_report_err(local_err);
                return 1;
            }
            object_ref(OBJECT(q->iothread));
            q->ctx = iothread_get_aio_context(q->iothread);
        } else {
            q->ctx = qemu_get_aio_context();
        }

        if (poll_max_ns != POLL_MAX_NS_UNSET) {
            aio_context_set_poll_params(q->ctx, poll_max_ns, 0, 0,
                                        &error_abort);
        }

        q->bh = aio_bh_new_guarded(q->ctx, bench_bh, q,
                                   &q->reentrancy_guard);

        /* per-queue buffer + request slots */
        size_t buf_bytes = (size_t)depth * bufsize;
        q->buf = blk_blockalign(blk, buf_bytes);
        memset(q->buf, pattern, buf_bytes);
        blk_register_buf(blk, q->buf, buf_bytes, &error_fatal);

        q->reqs = g_new0(struct bench_req, depth);
        q->freelist = g_new(struct bench_req *, depth);
        q->free_count = depth;
        for (int r = 0; r < depth; r++) {
            struct bench_req *req = &q->reqs[r];
            req->q = q;
            qemu_iovec_init(&req->qiov, 1);
            qemu_iovec_add(&req->qiov, q->buf + (size_t)r * bufsize, bufsize);
            q->freelist[r] = req;
        }
    }

    /* attach BB to queue[0]'s ctx (block-qcow2.c:607) */
    blk_set_aio_context(blk, qs[0].ctx, &error_abort);

    if (!quiet) {
        printf("Sending %d %s requests, %d bytes each, depth %d per queue, "
               "%d queues (offset=%" PRId64 ", step=%d)\n",
               count, is_write ? "write" : "read", bufsize, depth,
               n_queues, offset, step);
        printf("Model: %d producer thread(s) -> per-queue ring -> %s\n",
               n_queues,
               queues >= 1 ? "per-queue IOThread" : "main-loop AioContext");
        if (flush_interval) {
            printf("Flush every %d requests per queue%s\n", flush_interval,
                   drain_on_flush ? " (drained)" : "");
        }
        if (poll_max_ns != POLL_MAX_NS_UNSET) {
            printf("AioContext poll_max_ns overridden to %" PRId64 " ns\n",
                   poll_max_ns);
        }
        if (co_mutex_spin >= 0) {
            printf("CoMutex spin limit overridden to %d\n", co_mutex_spin);
        }
    }

    /* Spawn one producer pthread per queue. Each pushes into its own ring and
     * kicks its queue's BH; the consumer runs on the queue's IOThread ctx. */
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);
    for (int i = 0; i < n_queues; i++) {
        char name[32];
        snprintf(name, sizeof(name), "bench-prod%d", i);
        qemu_thread_create(&qs[i].producer, name, bench_producer, &qs[i],
                           QEMU_THREAD_JOINABLE);
    }

    while (qatomic_read(&shared.total_remaining) > 0) {
        main_loop_wait(false);
    }
    gettimeofday(&t2, NULL);

    for (int i = 0; i < n_queues; i++) {
        qemu_thread_join(&qs[i].producer);
    }

    double elapsed = (t2.tv_sec - t1.tv_sec)
                   + (double)(t2.tv_usec - t1.tv_usec) / 1000000.0;
    if (!quiet) {
        printf("Run completed in %.3f seconds.\n", elapsed);
        printf("  IOPS       : %.0f\n", count / elapsed);
        printf("  Throughput : %.2f MiB/s\n",
               (double)count * bufsize / (1024.0 * 1024.0) / elapsed);
        qcow2_counters_dump(stdout);
    }

    /* Teardown (mirrors block-qcow2.c:635 onward) */
    blk_set_aio_context(blk, qemu_get_aio_context(), &error_abort);
    blk_drain_all();

    for (int i = 0; i < n_queues; i++) {
        struct bench_queue *q = &qs[i];
        qemu_bh_cancel(q->bh);
        qemu_bh_delete(q->bh);
        for (int r = 0; r < depth; r++) {
            qemu_iovec_destroy(&q->reqs[r].qiov);
        }
        g_free(q->reqs);
        g_free(q->freelist);
        blk_unregister_buf(blk, q->buf, (size_t)depth * bufsize);
        qemu_vfree(q->buf);
        qemu_cond_destroy(&q->slot_avail);
        qemu_mutex_destroy(&q->lock);
        if (q->iothread) {
            object_unref(OBJECT(q->iothread));
            qmp_object_del(q->iothread_id, &local_err);
            if (local_err) {
                warn_report_err(local_err);
                local_err = NULL;
            }
            g_free(q->iothread_id);
        }
    }
    g_free(qs);

    blk_unref(blk);
    return ret;
}
