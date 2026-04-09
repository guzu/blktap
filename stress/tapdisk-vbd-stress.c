/*
 * tapdisk-vbd-stress
 *
 * Standalone micro-benchmark / stress harness for the tapdisk VBD I/O hot
 * path. It wires a synthetic "blkif" producer to a real td_vbd_t through the
 * real tapdisk scheduler, with a dummy block driver underneath that completes
 * every td_request_t synchronously.
 *
 * Goal: stress the VBD request issue path and the per-VBD pthread mutex
 * (td_vbd_handle.mutex) at very high request rates so that future
 * multi-threaded producers can be added on top with confidence.
 *
 * This is intentionally NOT a cmocka mockatest: it links against the real
 * libtapdisk.la and exercises the same code paths the in-tree tapdisk binary
 * uses. It is built as a noinst_PROGRAMS target and never installed.
 *
 * Single-threaded for now. The producer is registered as a 0-timeout timer
 * event so the scheduler drives it from its own loop; once we add a
 * multi-threaded producer, this same harness can be used to surface mutex
 * races on td_vbd_handle.mutex.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <unistd.h>

#include "list.h"
#include "scheduler.h"
#include "tapdisk.h"
#include "tapdisk-driver.h"
#include "tapdisk-image.h"
#include "tapdisk-interface.h"
#include "tapdisk-metrics.h"
#include "tapdisk-metrics-stats.h"
#include "tapdisk-server.h"
#include "tapdisk-vbd.h"
#include "timeout-math.h"

#define STRESS_DUMMY_TYPE     (-1)
#define STRESS_DUMMY_NAME     "dummy0"

/* ------------------------------------------------------------------------- */
/* Dummy block driver -- threaded, modeled after block-qcow2.c               */
/* ------------------------------------------------------------------------- */

/*
 * The driver runs as TD_DRIVER_THREADED. td_queue_read/write do *not*
 * complete the request synchronously; they enqueue a copy of the
 * td_request_t on an internal worker queue and return immediately.
 * A dedicated worker thread dequeues, sleeps the configured latency,
 * then calls td_complete_request followed by tapdisk_vbd_kick(vbd, true)
 * -- exactly the pattern signal_completion() uses in block-qcow2.c.
 *
 * This breaks the synchronous producer→completer inversion the harness
 * had until now, so vbd->mutex is contended for real: the producer
 * holds it inside tapdisk_vbd_issue_request while the worker tries to
 * acquire it inside __tapdisk_vbd_complete_td_request and tapdisk_vbd_kick.
 */

struct dummy_pending {
	td_request_t      treq;
	struct list_head  next;
};

static struct {
	pthread_t        thread;
	pthread_mutex_t  lock;
	pthread_cond_t   cond;
	struct list_head queue;
	bool             stop;
	bool             started;
	uint64_t         processed;
} dummy_worker;

static int
dummy_open(td_driver_t *driver, const char *name,
	   struct td_vbd_encryption *enc, td_flag_t flags)
{
	(void)driver;
	(void)name;
	(void)enc;
	(void)flags;
	return 0;
}

static int
dummy_close(td_driver_t *driver)
{
	(void)driver;
	return 0;
}

/* Forward decl: needs g_state.opts.latency_max_us, defined further down. */
static void dummy_inject_latency(void);

static void
dummy_enqueue(td_request_t treq)
{
	struct dummy_pending *p;

	p = malloc(sizeof(*p));
	if (!p) {
		fprintf(stderr, "stress: dummy_enqueue OOM\n");
		exit(1);
	}
	p->treq = treq;
	INIT_LIST_HEAD(&p->next);

	pthread_mutex_lock(&dummy_worker.lock);
	list_add_tail(&p->next, &dummy_worker.queue);
	pthread_cond_signal(&dummy_worker.cond);
	pthread_mutex_unlock(&dummy_worker.lock);
}

static void
dummy_queue_read(td_driver_t *driver, td_request_t treq)
{
	(void)driver;
	dummy_enqueue(treq);
}

static void
dummy_queue_write(td_driver_t *driver, td_request_t treq)
{
	(void)driver;
	dummy_enqueue(treq);
}

static void *
dummy_worker_fn(void *arg)
{
	(void)arg;

	for (;;) {
		struct dummy_pending *p;
		td_vbd_t *vbd;

		pthread_mutex_lock(&dummy_worker.lock);
		while (list_empty(&dummy_worker.queue) && !dummy_worker.stop)
			pthread_cond_wait(&dummy_worker.cond,
					  &dummy_worker.lock);
		if (list_empty(&dummy_worker.queue) && dummy_worker.stop) {
			pthread_mutex_unlock(&dummy_worker.lock);
			return NULL;
		}
		p = list_entry(dummy_worker.queue.next,
			       struct dummy_pending, next);
		list_del(&p->next);
		pthread_mutex_unlock(&dummy_worker.lock);

		dummy_inject_latency();

		/*
		 * Replicate signal_completion() from block-qcow2.c: complete
		 * the td_request, then kick the VBD so the eventfd wakes the
		 * scheduler thread (and so any vreqs whose iovs are now all
		 * back actually have their cb dispatched).
		 */
		vbd = p->treq.vreq->vbd;
		td_complete_request(p->treq, 0);
		tapdisk_vbd_kick(vbd, true);

		dummy_worker.processed++;
		free(p);
	}
}

static void
dummy_worker_start(void)
{
	int err;

	INIT_LIST_HEAD(&dummy_worker.queue);
	pthread_mutex_init(&dummy_worker.lock, NULL);
	pthread_cond_init(&dummy_worker.cond, NULL);

	err = pthread_create(&dummy_worker.thread, NULL,
			     dummy_worker_fn, NULL);
	if (err) {
		fprintf(stderr, "stress: pthread_create: %s\n", strerror(err));
		exit(1);
	}
	dummy_worker.started = true;
}

static void
dummy_worker_stop(void)
{
	if (!dummy_worker.started)
		return;

	pthread_mutex_lock(&dummy_worker.lock);
	dummy_worker.stop = true;
	pthread_cond_broadcast(&dummy_worker.cond);
	pthread_mutex_unlock(&dummy_worker.lock);

	pthread_join(dummy_worker.thread, NULL);
	dummy_worker.started = false;
}

static struct tap_disk dummy_tap_disk = {
	.disk_type          = "dummy",
	.flags              = TD_DRIVER_THREADED,
	.private_data_size  = 0,
	.td_open            = dummy_open,
	.td_close           = dummy_close,
	.td_queue_read      = dummy_queue_read,
	.td_queue_write     = dummy_queue_write,
};

/* ------------------------------------------------------------------------- */
/* Producer (fake blkif)                                                     */
/* ------------------------------------------------------------------------- */

struct stress_opts {
	uint64_t target;     /* total requests to issue */
	int      depth;      /* max in-flight at once */
	int      secs;       /* sectors per request */
	int      mode;       /* 0 read, 1 write, 2 mixed */
	int      verbose;
	uint64_t disk_secs;  /* virtual disk size in sectors */
	int      batch_max;  /* max submissions per producer tick */
	int      latency_max_us; /* max injected per-request latency in us */
	int      max_runtime_s;  /* hard wall-clock cap */
	int      iov_max;        /* max iovs per vreq, random in [1,N] */
	int      n_producers;    /* number of sequential producer events */
	int      list_period_ms; /* period of the simulated tap-ctl list, 0=off */
};

/*
 * Hard ceiling for the per-vreq iov array. Real blkif tops out at 11
 * (BLKIF_MAX_SEGMENTS_PER_REQUEST) but we leave some headroom so the
 * harness can deliberately exceed the kernel limit when stressing the
 * issue loop.
 */
#define STRESS_IOV_HARD_MAX      32
#define STRESS_PRODUCER_HARD_MAX 32

/*
 * One sequential producer in the harness. Each producer has a stable
 * `token` it stamps on every vreq it submits, so consecutive completions
 * coming from the same producer carry the same token. That is exactly
 * what tapdisk_vbd_kick groups on -- it walks the completed_requests
 * list and dispatches all entries sharing prev->token in one go before
 * moving on to the next token. With a single producer the grouping
 * never triggers (one token only); with N producers running in the
 * same thread, completions interleave and the grouping is exercised.
 */
struct producer {
	int          id;
	void        *token;     /* stable, identifies this producer */
	event_id_t   evid;
};

struct stress_state {
	struct stress_opts   opts;

	td_vbd_t            *vbd;
	td_image_t          *image;
	td_driver_t         *driver;
	stats_t              vdi_stats_storage;

	struct producer      producers[STRESS_PRODUCER_HARD_MAX];

	event_id_t           list_evid;

	uint64_t             submitted;
	uint64_t             completed;
	uint64_t             errors;
	int                  inflight;

	uint64_t             list_calls;   /* simulated tap-ctl list ticks */
	uint64_t             list_vbds;    /* total VBDs walked */

	bool                 stop_producer;

	struct timeval       t_start;
};

static double elapsed_secs(const struct timeval *start);

/*
 * Forward decl: defined in drivers/tapdisk-vbd.c but not exported in
 * the public header. It is the scheduler callback registered against
 * vbd->efd in the real tapdisk_vbd_open_vdi() path; we register the
 * same one here so the read-side semantics match exactly.
 */
extern void tapdisk_vbd_event_cb(event_id_t id, char mode, void *private);

static struct stress_state g_state;

static void
dummy_inject_latency(void)
{
	int max = g_state.opts.latency_max_us;
	int us;

	if (max <= 0)
		return;

	us = rand() % (max + 1);
	if (us > 0)
		usleep((useconds_t)us);
}

/*
 * Per-request scratch. We over-allocate so the buffer is large enough for
 * the largest sector count we will ever submit; the dummy driver does not
 * touch it but tapdisk_image_check_request does some sanity arithmetic.
 */
struct stress_req {
	td_vbd_request_t  vreq;
	struct td_iovec   iov[STRESS_IOV_HARD_MAX];
	int               iovcnt;
	char              name[32];
	void             *buf;       /* contiguous backing for all iovs */
};

static int next_op(void)
{
	switch (g_state.opts.mode) {
	case 0:  return TD_OP_READ;
	case 1:  return TD_OP_WRITE;
	default: return (g_state.submitted & 1) ? TD_OP_WRITE : TD_OP_READ;
	}
}

static void
stress_complete_cb(td_vbd_request_t *vreq, int error,
		   void *token, int final)
{
	/*
	 * vreq is the first field of struct stress_req, so a plain cast
	 * recovers the per-request scratch. We deliberately do *not* use
	 * vreq->token here -- it now identifies the producer, not the
	 * individual request.
	 */
	struct stress_req *r = (struct stress_req *)vreq;

	(void)token;
	(void)final;

	if (error)
		__atomic_add_fetch(&g_state.errors,    1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_state.completed, 1, __ATOMIC_RELAXED);
	__atomic_sub_fetch(&g_state.inflight,  1, __ATOMIC_RELAXED);

	free(r->buf);
	free(r);
}

static void
producer_submit_one(struct producer *prod)
{
	struct stress_req *r;
	uint64_t i;
	int iovcnt, k;
	size_t per_iov_bytes, total_bytes;
	int total_secs;
	char *p;

	r = calloc(1, sizeof(*r));
	if (!r) {
		fprintf(stderr, "stress: out of memory\n");
		exit(1);
	}

	/*
	 * Random iovcnt in [1, iov_max]. Each iov carries `secs` sectors so
	 * the per-vreq footprint is iovcnt * secs sectors. This is what
	 * actually exercises the per-iov loop in tapdisk_vbd_issue_request:
	 * a vreq with N iovs issues N td_request_t through td_queue_*, all
	 * under the same vbd->mutex hold.
	 */
	iovcnt = 1 + rand() % g_state.opts.iov_max;
	r->iovcnt    = iovcnt;
	total_secs   = iovcnt * g_state.opts.secs;
	per_iov_bytes = (size_t)g_state.opts.secs << SECTOR_SHIFT;
	total_bytes  = per_iov_bytes * (size_t)iovcnt;

	r->buf = calloc(1, total_bytes);
	if (!r->buf) {
		free(r);
		fprintf(stderr, "stress: out of memory (buf)\n");
		exit(1);
	}

	i = g_state.submitted;
	snprintf(r->name, sizeof(r->name), "s%" PRIu64, i);

	p = r->buf;
	for (k = 0; k < iovcnt; k++) {
		r->iov[k].base = p;
		r->iov[k].secs = g_state.opts.secs;
		p += per_iov_bytes;
	}

	r->vreq.op       = next_op();
	r->vreq.sec      = (i * total_secs) %
			   (g_state.opts.disk_secs - total_secs);
	r->vreq.iov      = r->iov;
	r->vreq.iovcnt   = iovcnt;
	r->vreq.cb       = stress_complete_cb;
	r->vreq.token    = prod->token;  /* per-producer; tapdisk_vbd_kick groups by this */
	r->vreq.name     = r->name;
	r->vreq.vbd      = g_state.vbd;
	INIT_LIST_HEAD(&r->vreq.next);

	__atomic_add_fetch(&g_state.submitted, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_state.inflight,  1, __ATOMIC_RELAXED);

	tapdisk_vbd_queue_request(g_state.vbd, &r->vreq);
}

static void
producer_event_cb(event_id_t id, char mode, void *private)
{
	struct producer *prod = private;
	int batch, n;

	(void)mode;

	if (g_state.opts.max_runtime_s > 0 &&
	    elapsed_secs(&g_state.t_start) >= g_state.opts.max_runtime_s)
		g_state.stop_producer = true;

	if (g_state.stop_producer)
		goto rearm;

	/*
	 * Submit a random batch in [1, batch_max] per tick. Modelling the
	 * producer as a fixed-rate drip is misleading: a real blkif feeds the
	 * VBD in bursts whose size depends on guest behaviour, and the size of
	 * each burst is exactly what determines how long vbd->mutex is held in
	 * tapdisk_vbd_issue_request's per-iov loop.
	 */
	batch = 1 + rand() % g_state.opts.batch_max;
	for (n = 0; n < batch; n++) {
		if (__atomic_load_n(&g_state.inflight, __ATOMIC_RELAXED) >=
		    g_state.opts.depth)
			break;
		if (g_state.opts.target > 0 &&
		    __atomic_load_n(&g_state.submitted, __ATOMIC_RELAXED) >=
		    g_state.opts.target)
			break;
		producer_submit_one(prod);
	}

rearm:
	/* Re-arm the timer event so we get called again on the next tick. */
	tapdisk_server_event_set_timeout(id, TV_ZERO);
}

/*
 * Simulated `tap-ctl list`. The real control handler walks
 * tapdisk_server_get_all_vbds() and reads vbd->name / vbd->state for
 * every VBD it finds. We replay the same access pattern from a periodic
 * scheduler event so the producer hot path is exercised under
 * concurrent reads of the global VBD list -- exactly what happens in
 * production whenever an operator runs `tap-ctl list` against a busy
 * tapdisk.
 */
static void
list_event_cb(event_id_t id, char mode, void *private)
{
	struct list_head *head;
	td_vbd_t *vbd;
	int count = 0;
	struct timeval tv;

	(void)mode;
	(void)private;

	head = tapdisk_server_get_all_vbds();
	list_for_each_entry(vbd, head, next) {
		/* Touch exactly the fields tapdisk_control_list reads, so
		 * the compiler cannot optimize the walk away. */
		volatile int   state = vbd->state;
		volatile char *name  = vbd->name;
		(void)state;
		(void)name;
		count++;
	}

	g_state.list_calls++;
	g_state.list_vbds += count;

	if (g_state.stop_producer)
		return;   /* let the event drain naturally */

	tv.tv_sec  =  g_state.opts.list_period_ms / 1000;
	tv.tv_usec = (g_state.opts.list_period_ms % 1000) * 1000;
	tapdisk_server_event_set_timeout(id, tv);
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                     */
/* ------------------------------------------------------------------------- */

static int
setup_dummy_image(void)
{
	td_driver_t *driver;
	td_image_t  *image;

	driver = calloc(1, sizeof(*driver));
	if (!driver)
		return -ENOMEM;

	driver->type        = STRESS_DUMMY_TYPE;
	driver->name        = strdup(STRESS_DUMMY_NAME);
	driver->ops         = &dummy_tap_disk;
	driver->refcnt      = 1;
	driver->state       = TD_DRIVER_OPEN;
	driver->info.size   = g_state.opts.disk_secs;
	driver->info.sector_size = DEFAULT_SECTOR_SIZE;
	driver->info.info   = 0;
	INIT_LIST_HEAD(&driver->next);

	image = tapdisk_image_allocate(STRESS_DUMMY_NAME,
				       STRESS_DUMMY_TYPE, 0);
	if (!image) {
		free(driver->name);
		free(driver);
		return -ENOMEM;
	}

	image->driver = driver;
	image->info   = driver->info;

	list_add_tail(&image->next, &g_state.vbd->images);

	g_state.driver = driver;
	g_state.image  = image;

	/* Make tapdisk_vbd_get_disk_info happy. */
	g_state.vbd->disk_info = driver->info;

	return 0;
}

static int
setup_vbd(void)
{
	int err;
	td_uuid_t uuid = 0xBEEF;

	err = tapdisk_vbd_initialize(-1, -1, uuid);
	if (err) {
		fprintf(stderr, "tapdisk_vbd_initialize: %s\n", strerror(-err));
		return err;
	}

	g_state.vbd = tapdisk_server_get_vbd(uuid);
	if (!g_state.vbd) {
		fprintf(stderr, "tapdisk_server_get_vbd returned NULL\n");
		return -ENOENT;
	}

	g_state.vbd->name = strdup("stress-vbd");

	/*
	 * tapdisk_vbd_issue_request() and __tapdisk_vbd_complete_td_request()
	 * unconditionally bump counters via vbd->vdi_stats.stats. Normally this
	 * is wired up by td_metrics_vdi_start() through the metrics shm. We do
	 * not need a real shm here -- a plain heap-backed struct stats is
	 * enough to keep the counter increments from NULL-derefing.
	 */
	memset(&g_state.vdi_stats_storage, 0, sizeof(g_state.vdi_stats_storage));
	g_state.vdi_stats_storage.stats = calloc(1, sizeof(struct stats));
	if (!g_state.vdi_stats_storage.stats)
		return -ENOMEM;
	g_state.vbd->vdi_stats = g_state.vdi_stats_storage;

	/*
	 * Wire up the TD_DRIVER_THREADED plumbing the same way
	 * tapdisk_vbd_open_vdi() does for qcow2: an eventfd that the
	 * worker thread bumps via tapdisk_vbd_kick(vbd, true), and a
	 * scheduler READ_FD event that drains it on the main thread so
	 * the next iterate() actually wakes up.
	 *
	 * setting vbd->driver_flags is what makes tapdisk_vbd_kick
	 * write to the eventfd at all.
	 */
	g_state.vbd->driver_flags = TD_DRIVER_THREADED;

	g_state.vbd->efd = eventfd(0, 0);
	if (g_state.vbd->efd < 0) {
		fprintf(stderr, "eventfd: %s\n", strerror(errno));
		return -errno;
	}

	g_state.vbd->event = tapdisk_server_register_event(
			SCHEDULER_POLL_READ_FD, g_state.vbd->efd, TV_INF,
			tapdisk_vbd_event_cb, g_state.vbd);
	if (g_state.vbd->event < 0) {
		fprintf(stderr, "register_event(efd): %s\n",
			strerror(-g_state.vbd->event));
		return g_state.vbd->event;
	}

	return setup_dummy_image();
}

/* ------------------------------------------------------------------------- */
/* Main loop / reporting                                                     */
/* ------------------------------------------------------------------------- */

static double
elapsed_secs(const struct timeval *start)
{
	struct timeval now, diff;
	gettimeofday(&now, NULL);
	timersub(&now, start, &diff);
	return (double)diff.tv_sec + (double)diff.tv_usec / 1e6;
}

static void
report(void)
{
	double secs = elapsed_secs(&g_state.t_start);
	const char *reason;

	if (g_state.opts.target > 0 &&
	    g_state.completed >= g_state.opts.target)
		reason = "target reached";
	else if (g_state.stop_producer)
		reason = "max runtime reached";
	else
		reason = "drained";

	printf("stress: stop reason: %s\n", reason);
	printf("stress: target=%" PRIu64 " submitted=%" PRIu64
	       " completed=%" PRIu64 " errors=%" PRIu64 "\n",
	       g_state.opts.target, g_state.submitted,
	       g_state.completed, g_state.errors);
	printf("stress: %.3fs elapsed, %.0f req/s\n",
	       secs, secs > 0 ? (double)g_state.completed / secs : 0.0);
	printf("stress: dummy worker processed %" PRIu64 " td_requests\n",
	       dummy_worker.processed);
	if (g_state.opts.list_period_ms > 0)
		printf("stress: simulated tap-ctl list: %" PRIu64
		       " calls, %" PRIu64 " VBDs walked\n",
		       g_state.list_calls, g_state.list_vbds);
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-n target] [-d depth] [-s sectors] [-m r|w|x]\n"
		"            [-b batch] [-l latency_us] [-T runtime_s]\n"
		"            [-i iov_max] [-p producers] [-L list_ms] [-v]\n"
		"  -n N    total requests, 0=unlimited "
				"(default 0; setting -n implies -T 0 unless -T is also given)\n"
		"  -d N    max in-flight (default 32)\n"
		"  -s N    sectors per iov (default 8)\n"
		"  -m M    r=read, w=write, x=mixed (default x)\n"
		"  -b N    max submissions per producer tick, "
				"random in [1,N] (default 32)\n"
		"  -l N    max injected dummy-driver latency in us, "
				"random in [0,N] (default 100)\n"
		"  -T N    hard wall-clock cap in seconds, 0=unlimited "
				"(default 30)\n"
		"  -i N    max iovs per vreq, random in [1,N] "
				"(default 4, hard max 32)\n"
		"  -p N    number of sequential producers in the same thread "
				"(default 1, hard max 32)\n"
		"  -L N    period in ms of a simulated `tap-ctl list` walk "
				"(default 1000, 0 = disabled)\n"
		"  -v      verbose\n",
		prog);
}

static void
parse_args(int argc, char **argv)
{
	int c;

	g_state.opts.target    = 0;        /* 0 = unlimited */
	g_state.opts.depth     = 32;
	g_state.opts.secs      = 8;
	g_state.opts.mode      = 2;
	g_state.opts.verbose   = 0;
	g_state.opts.disk_secs = 1ULL << 20;   /* 512MB virtual */
	g_state.opts.batch_max      = 32;
	g_state.opts.latency_max_us = 100;
	g_state.opts.max_runtime_s  = -1;  /* sentinel: resolved below */
	g_state.opts.iov_max        = 4;
	g_state.opts.n_producers    = 1;
	g_state.opts.list_period_ms = 1000;

	while ((c = getopt(argc, argv, "n:d:s:m:b:l:T:i:p:L:vh")) != -1) {
		switch (c) {
		case 'n': g_state.opts.target    = strtoull(optarg, NULL, 0); break;
		case 'd': g_state.opts.depth     = atoi(optarg); break;
		case 's': g_state.opts.secs      = atoi(optarg); break;
		case 'm':
			if (optarg[0] == 'r')      g_state.opts.mode = 0;
			else if (optarg[0] == 'w') g_state.opts.mode = 1;
			else                       g_state.opts.mode = 2;
			break;
		case 'b': g_state.opts.batch_max      = atoi(optarg); break;
		case 'l': g_state.opts.latency_max_us = atoi(optarg); break;
		case 'T': g_state.opts.max_runtime_s  = atoi(optarg); break;
		case 'i': g_state.opts.iov_max        = atoi(optarg); break;
		case 'p': g_state.opts.n_producers    = atoi(optarg); break;
		case 'L': g_state.opts.list_period_ms = atoi(optarg); break;
		case 'v': g_state.opts.verbose        = 1; break;
		case 'h':
		default:  usage(argv[0]); exit(c == 'h' ? 0 : 1);
		}
	}

	if (g_state.opts.depth          < 1) g_state.opts.depth          = 1;
	if (g_state.opts.secs           < 1) g_state.opts.secs           = 1;
	if (g_state.opts.batch_max      < 1) g_state.opts.batch_max      = 1;
	if (g_state.opts.latency_max_us < 0) g_state.opts.latency_max_us = 0;
	if (g_state.opts.iov_max        < 1) g_state.opts.iov_max        = 1;
	if (g_state.opts.iov_max > STRESS_IOV_HARD_MAX)
		g_state.opts.iov_max = STRESS_IOV_HARD_MAX;
	if (g_state.opts.n_producers    < 1) g_state.opts.n_producers    = 1;
	if (g_state.opts.n_producers > STRESS_PRODUCER_HARD_MAX)
		g_state.opts.n_producers = STRESS_PRODUCER_HARD_MAX;
	if (g_state.opts.list_period_ms < 0) g_state.opts.list_period_ms = 0;

	/*
	 * Resolve the -T sentinel. Default policy: time-bounded run with no
	 * request cap. As soon as the user explicitly asks for a request
	 * count via -n, drop the wall-clock cap so the run is bounded purely
	 * by -n. An explicit -T (including -T 0) always wins.
	 */
	if (g_state.opts.max_runtime_s < 0)
		g_state.opts.max_runtime_s = (g_state.opts.target > 0) ? 0 : 30;
}

int
main(int argc, char **argv)
{
	int err;

	parse_args(argc, argv);

	srand((unsigned int)(time(NULL) ^ getpid()));

	/*
	 * tapdisk_server_init() sets up PAGE_SIZE, the scheduler and the
	 * lowmem/cpumond clients. tapdisk_server_complete() additionally wires
	 * up the libaio backends and the tlog. We need the latter because
	 * tapdisk_server_iterate() unconditionally drives the rw/ro backends
	 * via tapdisk_server_submit_tiocbs() -- even though our dummy driver
	 * never queues a tiocb, the backend pointers must be non-NULL.
	 */
	err = tapdisk_server_init();
	if (err) {
		fprintf(stderr, "tapdisk_server_init: %s\n", strerror(-err));
		return 1;
	}

	err = tapdisk_server_complete();
	if (err) {
		fprintf(stderr, "tapdisk_server_complete: %s\n", strerror(-err));
		return 1;
	}

	err = setup_vbd();
	if (err) {
		fprintf(stderr, "setup_vbd: %s\n", strerror(-err));
		return 1;
	}

	if (g_state.opts.list_period_ms > 0) {
		struct timeval tv;
		tv.tv_sec  =  g_state.opts.list_period_ms / 1000;
		tv.tv_usec = (g_state.opts.list_period_ms % 1000) * 1000;
		g_state.list_evid = tapdisk_server_register_event(
				SCHEDULER_POLL_TIMEOUT, -1, tv,
				list_event_cb, NULL);
		if (g_state.list_evid < 0) {
			fprintf(stderr, "register_event(list): %s\n",
				strerror(-g_state.list_evid));
			return 1;
		}
	}

	for (int i = 0; i < g_state.opts.n_producers; i++) {
		struct producer *prod = &g_state.producers[i];

		prod->id    = i;
		prod->token = prod;     /* stable per-producer cookie */
		prod->evid  = tapdisk_server_register_event(
				SCHEDULER_POLL_TIMEOUT, -1, TV_ZERO,
				producer_event_cb, prod);
		if (prod->evid < 0) {
			fprintf(stderr, "register_event: %s\n",
				strerror(-prod->evid));
			return 1;
		}
	}

	dummy_worker_start();

	gettimeofday(&g_state.t_start, NULL);

	/*
	 * Custom drive loop. We bypass tapdisk_server_run() (which would
	 * install signal handlers and require a signalfd) and call iterate()
	 * directly. Two stop conditions:
	 *   - target reached: we got all completions we wanted
	 *   - max runtime reached: producer has stopped enqueuing AND we have
	 *     drained the in-flight queue, so the report is consistent
	 */
	for (;;) {
		if (g_state.opts.target > 0 &&
		    __atomic_load_n(&g_state.completed, __ATOMIC_RELAXED) >=
		    g_state.opts.target)
			break;
		if (g_state.stop_producer &&
		    __atomic_load_n(&g_state.inflight, __ATOMIC_RELAXED) == 0)
			break;
		tapdisk_server_iterate();
	}

	dummy_worker_stop();

	report();

	/*
	 * Sanity check: every submitted vreq must have come back through
	 * stress_complete_cb, otherwise the harness has either lost a
	 * completion (= bug in the VBD path) or torn down too early. The
	 * drive loop is supposed to drain in-flight before we get here, so
	 * a mismatch is fatal -- exit non-zero so the run is visible to CI.
	 */
	if (g_state.submitted != g_state.completed ||
	    g_state.inflight != 0) {
		fprintf(stderr,
			"stress: FAIL submitted=%" PRIu64
			" completed=%" PRIu64 " inflight=%d\n",
			g_state.submitted, g_state.completed,
			g_state.inflight);
		return 2;
	}

	/*
	 * Sanity check: at this point every vreq has been completed and
	 * the producer has stopped, so the per-VBD request lists must be
	 * empty. A non-empty list here means a vreq is stuck somewhere in
	 * the VBD state machine -- new (never issued), pending (issued but
	 * not completed by the driver), failed (waiting for retry), or
	 * completed (kicked but never reaped). All four are bugs.
	 */
	if (!list_empty(&g_state.vbd->new_requests) ||
	    !list_empty(&g_state.vbd->pending_requests) ||
	    !list_empty(&g_state.vbd->failed_requests) ||
	    !list_empty(&g_state.vbd->completed_requests)) {
		fprintf(stderr,
			"stress: FAIL vbd lists not empty:"
			" new=%d pending=%d failed=%d completed=%d\n",
			!list_empty(&g_state.vbd->new_requests),
			!list_empty(&g_state.vbd->pending_requests),
			!list_empty(&g_state.vbd->failed_requests),
			!list_empty(&g_state.vbd->completed_requests));
		return 2;
	}

	return 0;
}
