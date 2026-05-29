/*
 * qcow2-bench: benchmark tool reproducing block-qcow2.c's multi-iothread
 * dispatch model (1 BlockBackend, N IOThreads, BB attached to queue[0]'s
 * AioContext, each queue submits blk_aio_pre{ad,wr}itev from its own ctx).
 *
 * Modeled on qemu-img bench's request loop.
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
#include "qom/object_interfaces.h"

#include "block/aio.h"
#include "block/block.h"
#include "sysemu/block-backend.h"
#include "sysemu/iothread.h"

#define POLL_MAX_NS_UNSET (-1)

struct bench_shared {
    int total_remaining;   /* atomic: requests not yet completed (all queues) */
    AioContext *main_ctx;
};

struct bench_queue {
    /* shared with main thread (read-only after setup) */
    BlockBackend             *blk;
    uint64_t                  image_size;
    bool                      write;
    int                       bufsize;
    int                       step;          /* per-queue offset stride */
    int                       nrreq;         /* per-queue depth */
    int                       flush_interval;
    bool                      drain_on_flush;

    /* per-queue iothread plumbing */
    IOThread                 *iothread;
    char                     *iothread_id;
    AioContext               *ctx;
    QEMUBH                   *kick_bh;
    MemReentrancyGuard        reentrancy_guard;

    /* per-queue I/O state (mutated only on this->ctx) */
    int                       n;             /* requests not yet completed */
    int                       in_flight;
    bool                      in_flush;
    uint64_t                  offset;
    uint8_t                  *buf;            /* nrreq * bufsize, aligned */
    QEMUIOVector             *qiov;           /* nrreq entries */

    struct bench_shared      *shared;
    int                       id;
};

static void bench_cb(void *opaque, int ret);

static void bench_kick(void *opaque)
{
    struct bench_queue *q = opaque;
    bench_cb(q, 0);
}

static void bench_undrained_flush_cb(void *opaque, int ret)
{
    if (ret < 0) {
        error_report("queue: flush failed: %s", strerror(-ret));
        exit(EXIT_FAILURE);
    }
}

static void bench_cb(void *opaque, int ret)
{
    struct bench_queue *q = opaque;
    BlockAIOCB *acb;

    if (ret < 0) {
        error_report("queue %d: request failed: %s", q->id, strerror(-ret));
        exit(EXIT_FAILURE);
    }

    if (q->in_flush) {
        assert(q->in_flight == 0);
        q->in_flush = false;
    } else if (q->in_flight > 0) {
        int remaining = q->n - q->in_flight;

        q->n--;
        q->in_flight--;

        /* signal completion to main thread */
        if (qatomic_dec_fetch(&q->shared->total_remaining) == 0) {
            aio_notify(q->shared->main_ctx);
        }

        if (q->flush_interval && remaining % q->flush_interval == 0) {
            if (!q->in_flight || !q->drain_on_flush) {
                BlockCompletionFunc *cb;
                if (q->drain_on_flush) {
                    q->in_flush = true;
                    cb = bench_cb;
                } else {
                    cb = bench_undrained_flush_cb;
                }
                acb = blk_aio_flush(q->blk, cb, q);
                if (!acb) {
                    error_report("queue %d: failed to issue flush", q->id);
                    exit(EXIT_FAILURE);
                }
            }
            if (q->drain_on_flush) {
                return;
            }
        }
    }

    while (q->n > q->in_flight && q->in_flight < q->nrreq) {
        int slot = q->in_flight;
        int64_t off = q->offset;

        q->in_flight++;
        q->offset += q->step;
        if (q->image_size <= (uint64_t)q->bufsize) {
            q->offset = 0;
        } else {
            q->offset %= q->image_size - q->bufsize;
        }

        if (q->write) {
            acb = blk_aio_pwritev(q->blk, off, &q->qiov[slot], 0, bench_cb, q);
        } else {
            acb = blk_aio_preadv(q->blk, off, &q->qiov[slot], 0, bench_cb, q);
        }
        if (!acb) {
            error_report("queue %d: failed to issue request", q->id);
            exit(EXIT_FAILURE);
        }
    }
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
"  -d DEPTH          per-queue queue depth (default: 64)\n"
"  -j QUEUES         number of queues/iothreads (default: 1)\n"
"                    when >= 1, an IOThread is created per queue, matching\n"
"                    the drivers/block-qcow2.c model; 0 falls back to the\n"
"                    main loop AioContext (no IOThread).\n"
"  -s BUFSIZE        request size in bytes (default: 4096)\n"
"  -S STEPSIZE       per-queue offset stride (default: BUFSIZE * QUEUES)\n"
"  -o OFFSET         starting offset (default: 0)\n"
"  -w                write test (default: read)\n"
"  --pattern BYTE    pattern byte to write (default: 0)\n"
"  --flush-interval N   issue flush every N writes per queue\n"
"  --no-drain        don't drain queue before flush\n"
"  -t CACHE          cache mode (default: none)\n"
"  -i AIO            aio backend (threads, native, io_uring)\n"
"  -f FMT            image format (default: qcow2)\n"
"  --poll-max-ns N   AioContext adaptive poll window in ns (default:\n"
"                    upstream QEMU's IOTHREAD_POLL_MAX_NS_DEFAULT = 32768).\n"
"                    0 disables polling entirely (always blocks on ppoll).\n"
"                    Useful to investigate poll_grow/poll_shrink imbalance.\n"
"  -q                quiet\n"
"  -h                this help\n",
        prog);
}

enum {
    OPT_PATTERN = 256,
    OPT_FLUSH_INTERVAL,
    OPT_NO_DRAIN,
    OPT_POLL_MAX_NS,
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

    /* Create N IOThreads (or skip if queues==0) */
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
        q->nrreq       = depth;
        q->flush_interval = flush_interval;
        q->drain_on_flush = drain_on_flush;
        q->shared      = &shared;

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
            /* grow=0 / shrink=0 keep upstream defaults inside aio-posix:
             * grow factor of 2, shrink resets poll_ns to 0 on miss.
             */
            aio_context_set_poll_params(q->ctx, poll_max_ns, 0, 0,
                                        &error_abort);
        }

        q->kick_bh = aio_bh_new_guarded(q->ctx, bench_kick, q,
                                        &q->reentrancy_guard);

        /* per-queue buffer + qiov */
        size_t buf_bytes = (size_t)depth * bufsize;
        q->buf = blk_blockalign(blk, buf_bytes);
        memset(q->buf, pattern, buf_bytes);
        blk_register_buf(blk, q->buf, buf_bytes, &error_fatal);
        q->qiov = g_new(QEMUIOVector, depth);
        for (int r = 0; r < depth; r++) {
            qemu_iovec_init(&q->qiov[r], 1);
            qemu_iovec_add(&q->qiov[r], q->buf + r * bufsize, bufsize);
        }
    }

    /* attach BB to queue[0]'s ctx (block-qcow2.c:607) */
    blk_set_aio_context(blk, qs[0].ctx, &error_abort);

    if (!quiet) {
        printf("Sending %d %s requests, %d bytes each, depth %d per queue, "
               "%d queues (offset=%" PRId64 ", step=%d)\n",
               count, is_write ? "write" : "read", bufsize, depth,
               n_queues, offset, step);
        if (flush_interval) {
            printf("Flush every %d requests per queue\n", flush_interval);
        }
        if (poll_max_ns != POLL_MAX_NS_UNSET) {
            printf("AioContext poll_max_ns overridden to %" PRId64 " ns\n",
                   poll_max_ns);
        }
    }

    /* Kick each queue: bench_cb runs on its own iothread */
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);
    for (int i = 0; i < n_queues; i++) {
        qemu_bh_schedule(qs[i].kick_bh);
    }

    while (qatomic_read(&shared.total_remaining) > 0) {
        main_loop_wait(false);
    }
    gettimeofday(&t2, NULL);

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
        qemu_bh_cancel(q->kick_bh);
        qemu_bh_delete(q->kick_bh);
        for (int r = 0; r < depth; r++) {
            qemu_iovec_destroy(&q->qiov[r]);
        }
        g_free(q->qiov);
        blk_unregister_buf(blk, q->buf, (size_t)depth * bufsize);
        qemu_vfree(q->buf);
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
