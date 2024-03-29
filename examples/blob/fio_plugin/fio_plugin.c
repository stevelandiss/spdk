/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/copy_engine.h"
#include "spdk/conf.h"
#include "spdk/blob_bdev.h"
#include "spdk/lvol.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/queue.h"
#include "spdk/util.h"
#include "spdk/event.h"

#include "spdk_internal/thread.h"
#include "spdk_internal/lvolstore.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

/* FreeBSD is missing CLOCK_MONOTONIC_RAW,
 * so alternative is provided. */
#ifndef CLOCK_MONOTONIC_RAW /* Defined in glibc bits/time.h */
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

static bool g_blob_init_done = false;

struct spdk_fio_options {
	void *pad;
	char *conf;
	unsigned mem_mb;
	bool mem_single_seg;
};

struct spdk_fio_request {
	struct io_u		*io;
	struct thread_data	*td;
};

struct spdk_fio_target {
	struct spdk_bdev *bdev;
	struct spdk_bs_dev *bs_dev;
	struct spdk_blob_store *bs;
	struct spdk_bdev_desc *desc;
	struct spdk_blob *blob;
	struct spdk_io_channel *ch;
	spdk_blob_id blobid;
	uint64_t io_unit_size;

	uint8_t *write_buff;

	TAILQ_ENTRY(spdk_fio_target) link;
};

struct spdk_fio_thread {
	struct thread_data		*td; /* fio thread context */
	struct spdk_thread		*thread; /* spdk thread context */


	TAILQ_HEAD(, spdk_fio_target)	targets;
	bool				failed; /* true if the thread failed to initialize */

	struct io_u		**iocq;		/* io completion queue */
	unsigned int		iocq_count;	/* number of iocq entries filled by last getevents */
	unsigned int		iocq_size;	/* number of iocq entries allocated */
};

static bool g_spdk_env_initialized = false;
uint32_t blk_size, buf_align;

static int spdk_fio_init(struct thread_data *td);
static void spdk_fio_cleanup(struct thread_data *td);
static size_t spdk_fio_poll_thread(struct spdk_fio_thread *fio_thread);

/* Default polling timeout (ns) */
#define SPDK_FIO_POLLING_TIMEOUT 1000000000ULL

static int
spdk_fio_init_thread(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;

	fio_thread = calloc(1, sizeof(*fio_thread));
	if (!fio_thread) {
		SPDK_ERRLOG("failed to allocate thread local context\n");
		return -1;
	}

	fio_thread->td = td;
	td->io_ops_data = fio_thread;

	fio_thread->thread = spdk_thread_create("fio_thread", NULL);
	if (!fio_thread->thread) {
		free(fio_thread);
		SPDK_ERRLOG("failed to allocate thread\n");
		return -1;
	}
	spdk_set_thread(fio_thread->thread);

	fio_thread->iocq_size = td->o.iodepth;
	fio_thread->iocq = calloc(fio_thread->iocq_size, sizeof(struct io_u *));
	assert(fio_thread->iocq != NULL);

	TAILQ_INIT(&fio_thread->targets);

	return 0;
}

static void
unload_complete(void *cb_arg, int bserrno)
{
	if (bserrno) {
		fprintf(stderr, "error while unloading blob %d\n", bserrno);
		exit(-1);
	}

	fprintf(stderr, "blob unloaded\n");
}

static void
delete_complete(void *cb_arg, int bserrno)
{
	struct spdk_fio_target *target = cb_arg;

	if (bserrno) {
		fprintf(stderr, "error while deleting blob %d\n", bserrno);
		exit(-1);
	}

	spdk_bs_free_io_channel(target->ch);
	spdk_bs_unload(target->bs, unload_complete, target);
}

static void
delete_blob(void *cb_arg, int bserrno)
{
	struct spdk_fio_target *target = cb_arg;

	if (bserrno) {
		fprintf(stderr, "error while closing blob %d\n", bserrno);
		exit(-1);
	}

	spdk_bs_delete_blob(target->bs, target->blobid, delete_complete, target);
}

static void
spdk_fio_bdev_close_targets(void *arg)
{
	struct spdk_fio_thread *fio_thread = arg;
	struct spdk_fio_target *target, *tmp;


	fprintf(stderr, "spdk_fio_bdev_close_targets called on %p\n",
			fio_thread->thread);

	TAILQ_FOREACH_SAFE(target, &fio_thread->targets, link, tmp) {
		fprintf(stderr, "removing target %p\n", target);
		TAILQ_REMOVE(&fio_thread->targets, target, link);
		if (target->ch) {
			spdk_put_io_channel(target->ch);
			spdk_blob_close(target->blob, delete_blob, target);
		} else {
			fprintf(stderr, "Ignoring uninitialized target %p\n", target);
		}
	}

	fprintf(stderr, "spdk_fio_bdev_close_targets DONE on %p\n",
			fio_thread->thread);
}

static void
spdk_fio_cleanup_thread(struct spdk_fio_thread *fio_thread)
{
	spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_close_targets, fio_thread);

	fprintf(stderr, "spdk_fio_cleanup_thread called... %p\n", fio_thread->thread);
	while (!spdk_thread_is_idle(fio_thread->thread)) {
		spdk_fio_poll_thread(fio_thread);
	}

	fprintf(stderr, "Exit Pollers: %d\n", spdk_thread_has_pollers(fio_thread->thread));

	fprintf(stderr, "spdk_fio_cleanup_thread setting thread %p\n", fio_thread->thread);
	spdk_set_thread(fio_thread->thread);

	spdk_thread_exit(fio_thread->thread);
	spdk_thread_destroy(fio_thread->thread);
	free(fio_thread->iocq);
	free(fio_thread);
}

static void
spdk_fio_calc_timeout(struct spdk_fio_thread *fio_thread, struct timespec *ts)
{
	uint64_t timeout, now;

	if (spdk_thread_has_active_pollers(fio_thread->thread)) {
		return;
	}

	timeout = spdk_thread_next_poller_expiration(fio_thread->thread);
	now = spdk_get_ticks();

	if (timeout == 0) {
		timeout = now + (SPDK_FIO_POLLING_TIMEOUT * spdk_get_ticks_hz()) / SPDK_SEC_TO_NSEC;
	}

	if (timeout > now) {
		timeout = ((timeout - now) * SPDK_SEC_TO_NSEC) / spdk_get_ticks_hz() +
			  ts->tv_sec * SPDK_SEC_TO_NSEC + ts->tv_nsec;

		ts->tv_sec  = timeout / SPDK_SEC_TO_NSEC;
		ts->tv_nsec = timeout % SPDK_SEC_TO_NSEC;
	}
}

static pthread_t g_init_thread_id = 0;
static pthread_mutex_t g_init_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_init_cond;
static bool g_poll_loop = true;

static void
spdk_fio_bdev_init_done(void *cb_arg, int rc)
{
	*(bool *)cb_arg = true;
}

static void
spdk_fio_bdev_init_start(void *arg)
{
	bool *done = arg;

	/* Initialize the copy engine */
	spdk_copy_engine_initialize();

	/* Initialize the bdev layer */
	spdk_bdev_initialize(spdk_fio_bdev_init_done, done);
}

static void
spdk_fio_bdev_fini_done(void *cb_arg)
{
	*(bool *)cb_arg = true;
}

static void
spdk_fio_copy_fini_start(void *arg)
{
	bool *done = arg;

	spdk_copy_engine_finish(spdk_fio_bdev_fini_done, done);
}

static void
spdk_fio_bdev_fini_start(void *arg)
{
	bool *done = arg;

	spdk_bdev_finish(spdk_fio_copy_fini_start, done);
}

static void *
spdk_init_thread_poll(void *arg)
{
	struct spdk_fio_options		*eo = arg;
	struct spdk_fio_thread		*fio_thread;
	struct spdk_conf		*config;
	struct spdk_env_opts		opts;
	bool				done;
	int				rc;
	struct timespec			ts;
	struct thread_data		td = {};

	/* Create a dummy thread data for use on the initialization thread. */
	td.o.iodepth = 32;
	td.eo = eo;

	/* Parse the SPDK configuration file */
	eo = arg;
	if (!eo->conf || !strlen(eo->conf)) {
		SPDK_ERRLOG("No configuration file provided\n");
		rc = EINVAL;
		goto err_exit;
	}

	config = spdk_conf_allocate();
	if (!config) {
		SPDK_ERRLOG("Unable to allocate configuration file\n");
		rc = ENOMEM;
		goto err_exit;
	}

	rc = spdk_conf_read(config, eo->conf);
	if (rc != 0) {
		SPDK_ERRLOG("Invalid configuration file format\n");
		spdk_conf_free(config);
		goto err_exit;
	}
	if (spdk_conf_first_section(config) == NULL) {
		SPDK_ERRLOG("Invalid configuration file format\n");
		spdk_conf_free(config);
		rc = EINVAL;
		goto err_exit;
	}
	spdk_conf_set_as_default(config);

	/* Initialize the environment library */
	spdk_env_opts_init(&opts);
	opts.name = "fio";

	if (eo->mem_mb) {
		opts.mem_size = eo->mem_mb;
	}
	opts.hugepage_single_segments = eo->mem_single_seg;

	if (spdk_env_init(&opts) < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
		spdk_conf_free(config);
		rc = EINVAL;
		goto err_exit;
	}
	spdk_unaffinitize_thread();

	spdk_thread_lib_init(NULL, 0);

	/* Create an SPDK thread temporarily */
	rc = spdk_fio_init_thread(&td);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create initialization thread\n");
		goto err_exit;
	}

	fio_thread = td.io_ops_data;

	/* Initialize the bdev layer */
	done = false;
	spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_init_start, &done);

	do {
		spdk_fio_poll_thread(fio_thread);
	} while (!done);

	/*
	 * Continue polling until there are no more events.
	 * This handles any final events posted by pollers.
	 */
	while (spdk_fio_poll_thread(fio_thread) > 0) {};

	/* Set condition variable */
	pthread_mutex_lock(&g_init_mtx);
	pthread_cond_signal(&g_init_cond);

	while (g_poll_loop) {
		spdk_fio_poll_thread(fio_thread);

		clock_gettime(CLOCK_MONOTONIC, &ts);
		spdk_fio_calc_timeout(fio_thread, &ts);

		rc = pthread_cond_timedwait(&g_init_cond, &g_init_mtx, &ts);
		if (rc != ETIMEDOUT) {
			break;
		}
	}

	pthread_mutex_unlock(&g_init_mtx);

	/* Finalize the bdev layer */
	done = false;
	spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_fini_start, &done);

	do {
		spdk_fio_poll_thread(fio_thread);
	} while (!done && !spdk_thread_is_idle(fio_thread->thread));

	spdk_fio_cleanup_thread(fio_thread);

	pthread_exit(NULL);

err_exit:
	exit(rc);
	return NULL;
}

static int
spdk_fio_init_env(struct thread_data *td)
{
	pthread_condattr_t attr;
	int rc = -1;

	if (pthread_condattr_init(&attr)) {
		SPDK_ERRLOG("Unable to initialize condition variable\n");
		return -1;
	}

	if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
		SPDK_ERRLOG("Unable to initialize condition variable\n");
		goto out;
	}

	if (pthread_cond_init(&g_init_cond, &attr)) {
		SPDK_ERRLOG("Unable to initialize condition variable\n");
		goto out;
	}

	/*
	 * Spawn a thread to handle initialization operations and to poll things
	 * like the admin queues periodically.
	 */
	rc = pthread_create(&g_init_thread_id, NULL, &spdk_init_thread_poll, td->eo);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to spawn thread to poll admin queue. It won't be polled.\n");
	}

	/* Wait for background thread to advance past the initialization */
	pthread_mutex_lock(&g_init_mtx);
	pthread_cond_wait(&g_init_cond, &g_init_mtx);
	pthread_mutex_unlock(&g_init_mtx);
out:
	pthread_condattr_destroy(&attr);
	return rc;
}

/* Called for each thread to fill in the 'real_file_size' member for
 * each file associated with this thread. This is called prior to
 * the init operation (spdk_fio_init()) below. This call will occur
 * on the initial start up thread if 'create_serialize' is true, or
 * on the thread actually associated with 'thread_data' if 'create_serialize'
 * is false.
 */
static int
spdk_fio_setup(struct thread_data *td)
{
	unsigned int i;
	struct fio_file *f;

	if (!td->o.use_thread) {
		SPDK_ERRLOG("must set thread=1 when using spdk plugin\n");
		return -1;
	}

	if (!g_spdk_env_initialized) {
		if (spdk_fio_init_env(td)) {
			SPDK_ERRLOG("failed to initialize\n");
			return -1;
		}

		g_spdk_env_initialized = true;
	}

	for_each_file(td, f, i) {
		struct spdk_bdev *bdev;

		bdev = spdk_bdev_get_by_name("AIO0");
		if (!bdev) {
			SPDK_ERRLOG("1. Unable to find bdev with name %s\n", f->file_name);
			return -1;
		}

		f->real_file_size = spdk_bdev_get_num_blocks(bdev) *
				    spdk_bdev_get_block_size(bdev);
	}

	return 0;
}

static void
sync_complete(void *cb_arg, int bserrno)
{
	struct spdk_fio_target *target = cb_arg;

	if (bserrno) {
		fprintf(stderr, "blob sync error %d\n", bserrno);
		exit(-1);
	}

	fprintf(stderr, "blob sync complete\n");

	target->ch = spdk_bs_alloc_io_channel(target->bs);

	target->write_buff = spdk_malloc(target->io_unit_size,
			0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
			SPDK_MALLOC_DMA);
	if (target->write_buff == NULL) {
		fprintf(stderr, "Could not allocate write buffer\n");
		exit(-1);
	}

	g_blob_init_done = true;
}

static void
resize_complete(void *cb_arg, int bserrno)
{
	struct spdk_fio_target *target = cb_arg;
	uint64_t total;

	if (bserrno) {
		fprintf(stderr, "blob resize error %d\n", bserrno);
		exit(-1);
	}

	fprintf(stderr, "blob resize complete\n");

	total = spdk_blob_get_num_clusters(target->blob);
	fprintf(stderr, "resized blobstore has %lu used clusters\n", total);

	spdk_blob_sync_md(target->blob, sync_complete, target);
}

static void
open_complete(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct spdk_fio_target *target = cb_arg;
	uint64_t free;

	if (bserrno) {
		fprintf(stderr, "blob open error %d\n", bserrno);
		exit(-1);
	}

	fprintf(stderr, "blob open complete\n");

	target->blob = blob;
	free = spdk_bs_free_cluster_count(target->bs);

	fprintf(stderr, "blobstore has %lu free clusters\n", free);

	spdk_blob_resize(target->blob, free, resize_complete, target);
}

static void
blob_create_complete(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
	struct spdk_fio_target *target = cb_arg;

	if (bserrno) {
		fprintf(stderr, "blob create error %d\n", bserrno);
		exit(-1);
	}

	fprintf(stderr, "blob create complete\n");

	target->blobid = blobid;

	spdk_bs_open_blob(target->bs, target->blobid, open_complete, target);
}

static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs, int bserrno)
{
	struct spdk_fio_target *target = cb_arg;

	if (bserrno) {
		fprintf(stderr, "blob store init error %d\n", bserrno);
		exit(-1);
	}

	fprintf(stderr, "blob store init complete\n");
	target->bs = bs;

	target->io_unit_size = spdk_bs_get_io_unit_size(target->bs);

	fprintf(stderr, "blob store IO unit size is %lu\n", target->io_unit_size);

	spdk_bs_create_blob(target->bs, blob_create_complete, target);
}

static void
spdk_fio_bdev_open(void *arg)
{
	struct thread_data *td = arg;
	struct spdk_fio_thread *fio_thread;
	unsigned int i;
	struct fio_file *f;
	int rc;

	fio_thread = td->io_ops_data;

	for_each_file(td, f, i) {
		struct spdk_fio_target *target;

		target = calloc(1, sizeof(*target));
		if (!target) {
			SPDK_ERRLOG("Unable to allocate memory for I/O target.\n");
			fio_thread->failed = true;
			return;
		}

		target->bdev = spdk_bdev_get_by_name("AIO0");
		if (!target->bdev) {
			SPDK_ERRLOG("2. Unable to find bdev with name %s\n", f->file_name);
			free(target);
			fio_thread->failed = true;
			return;
		}

		blk_size = spdk_bdev_get_block_size(target->bdev);
		buf_align = spdk_bdev_get_buf_align(target->bdev);

		rc = spdk_bdev_open(target->bdev, true, NULL, NULL, &target->desc);
		if (rc) {
			SPDK_ERRLOG("Unable to open bdev %s\n", f->file_name);
			free(target);
			fio_thread->failed = true;
			return;
		}

		fprintf(stderr, "block size = %u\nnumber of blocks = %lu\n",
				spdk_bdev_get_block_size(target->bdev),
				spdk_bdev_get_num_blocks(target->bdev));

		// BLOB init
		target->bs_dev = spdk_bdev_create_bs_dev(target->bdev, NULL, NULL);
		if (target->bs_dev == NULL) {
			SPDK_ERRLOG("Could not create g_bs_dev\n");
			spdk_bdev_close(target->desc);
			return;
		}

		struct spdk_bs_opts opts = {};

		spdk_bs_opts_init(&opts);
		// opts.cluster_sz = 4*1024*1024L;

		fprintf(stderr, "BS (%s) opts MD pages %d MD ops %d channel ops %d\n",
				opts.bstype.bstype, opts.num_md_pages, opts.max_md_ops, opts.max_channel_ops);

		spdk_bs_init(target->bs_dev, &opts, bs_init_complete, target);

		f->engine_data = target;

		TAILQ_INSERT_TAIL(&fio_thread->targets, target, link);
	}
}

/* Called for each thread, on that thread, shortly after the thread
 * starts.
 */
static int
spdk_fio_init(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;

	spdk_fio_init_thread(td);

	fio_thread = td->io_ops_data;
	fio_thread->failed = false;

	spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_open, td);

	while (spdk_fio_poll_thread(fio_thread) > 0) {}

	while (g_blob_init_done == false) {
		spdk_fio_poll_thread(fio_thread);
	}

	fprintf(stderr, "Backend is initialized... %d\n", g_blob_init_done);

	if (fio_thread->failed) {
		return -1;
	}

	return 0;
}

static void
spdk_fio_cleanup(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	fprintf(stderr, "FIO cleanup called %p\n", fio_thread);

	spdk_fio_cleanup_thread(fio_thread);
	td->io_ops_data = NULL;
}

static int
spdk_fio_open(struct thread_data *td, struct fio_file *f)
{

	return 0;
}

static int
spdk_fio_close(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int
spdk_fio_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	td->orig_buffer = spdk_dma_zmalloc(total_mem, 0x1000, NULL);
	return td->orig_buffer == NULL;
}

static void
spdk_fio_iomem_free(struct thread_data *td)
{
	spdk_dma_free(td->orig_buffer);
}

static int
spdk_fio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_request	*fio_req;

	fio_req = calloc(1, sizeof(*fio_req));
	if (fio_req == NULL) {
		return 1;
	}
	fio_req->io = io_u;
	fio_req->td = td;

	io_u->engine_data = fio_req;

	return 0;
}

static void
spdk_fio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_request *fio_req = io_u->engine_data;

	if (fio_req) {
		assert(fio_req->io == io_u);
		free(fio_req);
		io_u->engine_data = NULL;
	}
}

static void
spdk_fio_completion_cb(struct spdk_bdev_io *bdev_io,
		       bool success,
		       void *cb_arg)
{
	struct spdk_fio_request		*fio_req = cb_arg;
	struct thread_data		*td = fio_req->td;
	struct spdk_fio_thread		*fio_thread = td->io_ops_data;

	assert(fio_thread->iocq_count < fio_thread->iocq_size);
	fio_req->io->error = success ? 0 : EIO;
	fio_thread->iocq[fio_thread->iocq_count++] = fio_req->io;

	spdk_bdev_free_io(bdev_io);
}

#if FIO_IOOPS_VERSION >= 24
typedef enum fio_q_status fio_q_status_t;
#else
typedef int fio_q_status_t;
#endif

static void write_complete(
		void *arg,
		int error)
{
	struct spdk_fio_request	*fio_req = arg;
	struct thread_data *td = fio_req->td;
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	assert(error == 0);

	assert(fio_thread->iocq_count < fio_thread->iocq_size);

	fio_req->io->error = error ? EIO : 0;

	// XXX FIXME This can race with spdk_fio_getevents where iocq_count is set to zero
	fio_thread->iocq[fio_thread->iocq_count++] = fio_req->io;
}

static fio_q_status_t
spdk_fio_queue(struct thread_data *td, struct io_u *io_u)
{
	int rc = 1;
	struct spdk_fio_request	*fio_req = io_u->engine_data;
	struct spdk_fio_target *target = io_u->file->engine_data;
	uint64_t offset;
	uint64_t length;

	assert(fio_req->td == td);

	switch (io_u->ddir) {
	case DDIR_READ:
		rc = spdk_bdev_read(target->desc, target->ch,
				    io_u->buf, io_u->offset, io_u->xfer_buflen,
				    spdk_fio_completion_cb, fio_req);
		break;
	case DDIR_WRITE:
		rc = 0;

		offset = io_u->offset / target->io_unit_size;
		fprintf(stderr, "write at %lu\n", offset);

		length = io_u->xfer_buflen / target->io_unit_size;

		spdk_blob_io_write(
			target->blob,
			target->ch,
			io_u->buf,
			offset,
			length,
			write_complete,
			fio_req);

		break;
	case DDIR_TRIM:
		rc = spdk_bdev_unmap(target->desc, target->ch,
				     io_u->offset, io_u->xfer_buflen,
				     spdk_fio_completion_cb, fio_req);
		break;
	default:
		fprintf(stderr, "sync called\n");
		rc = spdk_bdev_flush(target->desc, target->ch,
				     io_u->offset, io_u->xfer_buflen,
				     spdk_fio_completion_cb, fio_req);
		rc = 0;
		break;
	}

	if (rc == -ENOMEM) {
		return FIO_Q_BUSY;
	}

	if (rc != 0) {
		fio_req->io->error = abs(rc);
		return FIO_Q_COMPLETED;
	}

	return FIO_Q_QUEUED;
}

static struct io_u *
spdk_fio_event(struct thread_data *td, int event)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	assert(event >= 0);
	assert((unsigned)event < fio_thread->iocq_count);

	assert(fio_thread->iocq[event] != 0);

	return fio_thread->iocq[event];
}

static size_t
spdk_fio_poll_thread(struct spdk_fio_thread *fio_thread)
{
	return spdk_thread_poll(fio_thread->thread, 0, 0);
}

static int
spdk_fio_getevents(struct thread_data *td, unsigned int min,
		   unsigned int max, const struct timespec *t)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct timespec t0, t1;
	uint64_t timeout = 0;

	if (t) {
		timeout = t->tv_sec * SPDK_SEC_TO_NSEC + t->tv_nsec;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	}

	fio_thread->iocq_count = 0;

	for (;;) {
		spdk_fio_poll_thread(fio_thread);

		if (fio_thread->iocq_count >= min) {
			goto done;
		}

		if (t) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
			uint64_t elapse = ((t1.tv_sec - t0.tv_sec) * SPDK_SEC_TO_NSEC)
					  + t1.tv_nsec - t0.tv_nsec;
			if (elapse > timeout) {
				break;
			}
		}
	}

done:
	return fio_thread->iocq_count;
}

static int
spdk_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* TODO: This should probably send a flush to the device, but for now just return successful. */
	return 0;
}

static struct fio_option options[] = {
	{
		.name		= "spdk_conf",
		.lname		= "SPDK configuration file",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, conf),
		.help		= "A SPDK configuration file",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "spdk_mem",
		.lname		= "SPDK memory in MB",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, mem_mb),
		.help		= "Amount of memory in MB to allocate for SPDK",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "spdk_single_seg",
		.lname		= "SPDK switch to create just a single hugetlbfs file",
		.type		= FIO_OPT_BOOL,
		.off1		= offsetof(struct spdk_fio_options, mem_single_seg),
		.help		= "If set to 1, SPDK will use just a single hugetlbfs file",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= NULL,
	},
};

/* FIO imports this structure using dlsym */
struct ioengine_ops ioengine = {
	.name			= "spdk_bdev",
	.version		= FIO_IOOPS_VERSION,
	.flags			= FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_MEMALIGN,
	.setup			= spdk_fio_setup,
	.init			= spdk_fio_init,
	/* .prep		= unused, */
	.queue			= spdk_fio_queue,
	/* .commit		= unused, */
	.getevents		= spdk_fio_getevents,
	.event			= spdk_fio_event,
	/* .errdetails		= unused, */
	/* .cancel		= unused, */
	.cleanup		= spdk_fio_cleanup,
	.open_file		= spdk_fio_open,
	.close_file		= spdk_fio_close,
	.invalidate		= spdk_fio_invalidate,
	/* .unlink_file		= unused, */
	/* .get_file_size	= unused, */
	/* .terminate		= unused, */
	.iomem_alloc		= spdk_fio_iomem_alloc,
	.iomem_free		= spdk_fio_iomem_free,
	.io_u_init		= spdk_fio_io_u_init,
	.io_u_free		= spdk_fio_io_u_free,
	.option_struct_size	= sizeof(struct spdk_fio_options),
	.options		= options,
};

static void fio_init spdk_fio_register(void)
{
	register_ioengine(&ioengine);
}

static void
spdk_fio_finish_env(void)
{
	pthread_mutex_lock(&g_init_mtx);
	g_poll_loop = false;
	pthread_cond_signal(&g_init_cond);
	pthread_mutex_unlock(&g_init_mtx);
	pthread_join(g_init_thread_id, NULL);

	spdk_thread_lib_fini();
}

static void fio_exit spdk_fio_unregister(void)
{
	if (g_spdk_env_initialized) {
		spdk_fio_finish_env();
		g_spdk_env_initialized = false;
	}
	unregister_ioengine(&ioengine);
}
