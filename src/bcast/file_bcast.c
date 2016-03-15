/*****************************************************************************\
 *  file_bcast.c - File transfer agent (handles message traffic)
 *****************************************************************************
 *  Copyright (C) 2015-2016 SchedMD LLC.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Copyright (C) 2006-2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if HAVE_LIBZ
# include <zlib.h>
#endif

#if HAVE_LZ4
# include <lz4.h>
#endif

#include "slurm/slurm_errno.h"
#include "src/common/forward.h"
#include "src/common/hostlist.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/slurm_protocol_interface.h"
#include "src/common/slurm_time.h"
#include "src/common/timers.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "file_bcast.h"

#define MAX_RETRIES     10
#define MAX_THREADS      8	/* These can be huge messages, so
				 * only run MAX_THREADS at one time */
typedef struct thd {
	pthread_t thread;	/* thread ID */
	slurm_msg_t msg;	/* message to send */
	int rc;			/* highest return codes from RPC */
	char *nodelist;
} thd_t;

int block_len;				/* block size */
int fd;					/* source file descriptor */
void *src;				/* source mmap'd address */
struct stat f_stat;			/* source file stats */
job_sbcast_cred_msg_t *sbcast_cred;	/* job alloc info and sbcast cred */

static int   _bcast_file(struct bcast_parameters *params);
static int   _file_bcast(struct bcast_parameters *params,
			 file_bcast_msg_t *bcast_msg,
			 job_sbcast_cred_msg_t *sbcast_cred);
static int   _file_state(struct bcast_parameters *params);
static int  _get_job_info(struct bcast_parameters *params);


static int _file_state(struct bcast_parameters *params)
{
	/* validate the source file */
	if ((fd = open(params->src_fname, O_RDONLY)) < 0) {
		error("Can't open `%s`: %s", params->src_fname,
			strerror(errno));
		return SLURM_ERROR;
	}
	if (fstat(fd, &f_stat)) {
		error("Can't stat `%s`: %s", params->src_fname,
			strerror(errno));
		return SLURM_ERROR;
	}

	src = mmap(NULL, f_stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (src < 0) {
		error("Can't mmap file `%s`, %m.", params->src_fname);
		return SLURM_ERROR;
	}

	verbose("modes    = %o", (unsigned int) f_stat.st_mode);
	verbose("uid      = %d", (int) f_stat.st_uid);
	verbose("gid      = %d", (int) f_stat.st_gid);
	verbose("atime    = %s", slurm_ctime2(&f_stat.st_atime));
	verbose("mtime    = %s", slurm_ctime2(&f_stat.st_mtime));
	verbose("ctime    = %s", slurm_ctime2(&f_stat.st_ctime));
	verbose("size     = %ld", (long) f_stat.st_size);

	return SLURM_SUCCESS;
}

/* get details about this slurm job: jobid and allocated node */
static int _get_job_info(struct bcast_parameters *params)
{
	int rc;

	xassert(params->job_id != NO_VAL);

	rc = slurm_sbcast_lookup(params->job_id, params->step_id, &sbcast_cred);
	if (rc != SLURM_SUCCESS) {
		if (params->step_id == NO_VAL) {
			error("Slurm job ID %u lookup error: %s",
			      params->job_id,
			      slurm_strerror(slurm_get_errno()));
		} else {
			error("Slurm step ID %u.%u lookup error: %s",
			      params->job_id, params->step_id,
			      slurm_strerror(slurm_get_errno()));
		}
		return rc;
	}
	if (params->step_id == NO_VAL)
		verbose("jobid      = %u", params->job_id);
	else
		verbose("stepid     = %u.%u", params->job_id, params->step_id);
	verbose("node_cnt   = %u", sbcast_cred->node_cnt);
	verbose("node_list  = %s", sbcast_cred->node_list);
	/* also see sbcast_cred->node_addr (array) */

	if (params->verbose)
		print_sbcast_cred(sbcast_cred->sbcast_cred);

	/* do not bother to release the return message,
	 * we need to preserve and use most of the information later */

	return rc;
}

/* Issue the RPC to transfer the file's data */
static int _file_bcast(struct bcast_parameters *params,
		       file_bcast_msg_t *bcast_msg,
		       job_sbcast_cred_msg_t *sbcast_cred)
{
	List ret_list = NULL;
	ListIterator itr;
	ret_data_info_t *ret_data_info = NULL;
	int rc = 0, msg_rc;
	slurm_msg_t msg;

	slurm_msg_t_init(&msg);
	msg.data = bcast_msg;
	msg.msg_type = REQUEST_FILE_BCAST;

	ret_list = slurm_send_recv_msgs(
		sbcast_cred->node_list, &msg, params->timeout, true);
	if (ret_list == NULL) {
		error("slurm_send_recv_msgs: %m");
		exit(1);
	}

	itr = list_iterator_create(ret_list);
	while ((ret_data_info = list_next(itr))) {
		msg_rc = slurm_get_return_code(ret_data_info->type,
					       ret_data_info->data);
		if (msg_rc == SLURM_SUCCESS)
			continue;

		error("REQUEST_FILE_BCAST(%s): %s",
		      ret_data_info->node_name,
		      slurm_strerror(msg_rc));
		rc = MAX(rc, msg_rc);
	}
	list_iterator_destroy(itr);
	FREE_NULL_LIST(ret_list);

	return rc;
}

/* load a buffer with data from the file to broadcast,
 * return number of bytes read, zero on end of file */
static int32_t _get_block_none(char **buffer, bool *more)
{
	static int remaining = -1;
	static void *position;
	int size;

	if (remaining < 0) {
		*buffer = xmalloc(block_len);
		remaining = f_stat.st_size;
		position = src;
	}

	size = MIN(block_len, remaining);
	if (size) {
		memcpy(*buffer, position, size);
		remaining -= size;
		position += size;
	}

	*more = (remaining) ? true : false;
	return size;
}

static int32_t _get_block_zlib(struct bcast_parameters *params,
			       char **buffer,
			       int32_t *orig_len,
			       bool *more)
{
#if HAVE_LIBZ
	static z_stream strm;
	int chunk = (256 * 1024);
	int flush = Z_NO_FLUSH;

	static int remaining = -1;
	static int max_out;
	static void *position;
	int chunk_remaining, out_remaining, chunk_bite, size = 0;

	/* allocate deflate state, compress each block independently */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) {
		error("File compression configuration error,"
		      "sending uncompressed file.");
		params->compress = 0;
		return _get_block_none(buffer, more);
	}

	/* first pass through, initialize */
	if (remaining < 0) {
		remaining = f_stat.st_size;
		max_out = block_len + 1024;
		*buffer = xmalloc(max_out);
		position = src;
	}

	if (remaining) {
		chunk_remaining = MIN(chunk, remaining);
		out_remaining = max_out;
		strm.next_out = (void *) *buffer;
		do {
			strm.next_in = position;
			chunk_bite = MIN(chunk, chunk_remaining);
			strm.avail_in = chunk_bite;
			strm.avail_out = out_remaining;

			if (chunk_remaining <= chunk)
				flush = Z_FINISH;

			if (deflate(&strm, flush) == Z_STREAM_ERROR)
				fatal("Error compressing file");

			position += chunk_bite;
			size += chunk_bite;
			chunk_remaining -= chunk_bite;
			out_remaining = strm.avail_out;
		} while (chunk_remaining);
		remaining -= size;
	}
	(void) deflateEnd(&strm);

	*orig_len = size;
	*more = (remaining) ? true : false;
	return (max_out - out_remaining);
#else
	info("zlib compression not supported, sending uncompressed file.");
	params->compress = 0;
	return _get_block_none(buffer, more);
#endif
}

static int32_t _get_block_lz4(struct bcast_parameters *params,
			      char **buffer,
			      int32_t *orig_len,
			      bool *more)
{
#if HAVE_LZ4
	int size_out;
	static int remaining = -1;
	static int max_out = -1;
	static void *position;
	int size;

	if (remaining < 0) {
		position = src;
		//XXX other LZ4 init goes here
		remaining = f_stat.st_size;
		max_out = LZ4_compressBound(block_len);
		*buffer = xmalloc(max_out);
	}

	size = MIN(block_len, remaining);
	if (size) {
		if (!(size_out = LZ4_compress_default(position, *buffer, size,
						      max_out))) {
			/* compression failure */
			fatal("LZ4 compression error");
		}
		position += size;
		remaining -= size;
	}

	*orig_len = size;
	*more = (remaining) ? true : false;
	return size_out;
#else
	info("lz4 compression not supported, sending uncompressed file.");
	params->compress = 0;
	return _get_block_none(buffer, more);
#endif

}

static int _next_block(struct bcast_parameters *params,
		       char **buffer,
		       int32_t *orig_len,
		       bool *more)
{
	switch(params->compress) {
	case COMPRESS_OFF:
		return _get_block_none(buffer, more);
	case COMPRESS_ZLIB:
		return _get_block_zlib(params, buffer, orig_len, more);
	case COMPRESS_LZ4:
		return _get_block_lz4(params, buffer, orig_len, more);
	}

	/* compression type not recognized */
	error("File compression type %u not supported,"
	      " sending uncompressed file.", params->compress);
	params->compress = 0;
	return _get_block_none(buffer, more);
}

/* read and broadcast the file */
static int _bcast_file(struct bcast_parameters *params)
{
	int rc = SLURM_SUCCESS;
	file_bcast_msg_t bcast_msg;
	char *buffer = NULL;
	int32_t orig_len;
	uint32_t size_uncompressed = 0, size_compressed = 0;
	uint32_t time_compression = 0;
	bool more = true;
	DEF_TIMERS;

	if (params->block_size)
		block_len = MIN(params->block_size, f_stat.st_size);
	else
		block_len = MIN((512 * 1024), f_stat.st_size);

	bzero(&bcast_msg, sizeof(file_bcast_msg_t));
	bcast_msg.fname		= params->dst_fname;
	bcast_msg.block_no	= 1;
	bcast_msg.force		= params->force;
	bcast_msg.modes		= f_stat.st_mode;
	bcast_msg.uid		= f_stat.st_uid;
	bcast_msg.user_name	= uid_to_string(f_stat.st_uid);
	bcast_msg.gid		= f_stat.st_gid;
	bcast_msg.cred          = sbcast_cred->sbcast_cred;

	if (params->preserve) {
		bcast_msg.atime     = f_stat.st_atime;
		bcast_msg.mtime     = f_stat.st_mtime;
	}

	if (!params->fanout)
		params->fanout = MAX_THREADS;
	slurm_set_tree_width(MIN(MAX_THREADS, params->fanout));

	while (more) {
		START_TIMER;
		bcast_msg.block_len = _next_block(params, &buffer, &orig_len,
						  &more);
		END_TIMER;
		time_compression += DELTA_TIMER;
		size_uncompressed += orig_len;
		size_compressed += bcast_msg.block_len;
		debug("block %d, size %u", bcast_msg.block_no,
		      bcast_msg.block_len);
		bcast_msg.compress = params->compress;
		bcast_msg.uncomp_len = orig_len;
		bcast_msg.block = buffer;
		if (!more)
			bcast_msg.last_block = 1;

		rc = _file_bcast(params, &bcast_msg, sbcast_cred);
		if (rc != SLURM_SUCCESS)
			break;
		if (bcast_msg.last_block)
			break;	/* end of file */
		bcast_msg.block_no++;
	}
	xfree(bcast_msg.user_name);
	xfree(buffer);

	if (size_uncompressed && params->compress != 0) {
		int32_t pct = (int32_t)(size_uncompressed - size_compressed);
		/* Dividing a negative by a positive in C99 results in
		 * "truncation towards zero" which gives unexpected values for
		 * pct. This construct avoids that problem.
		 */
		pct = (pct>=0) ? pct * 100 / size_uncompressed
			       : - (-pct * 100 / size_uncompressed);
		verbose("File compressed from %u to %u (%d percent) in %u usec",
			size_uncompressed, size_compressed, pct,
			time_compression);
	}

	return rc;
}

extern int bcast_file(struct bcast_parameters *params)
{
	int rc;

	if ((rc = _file_state(params)) != SLURM_SUCCESS)
		return rc;
	if ((rc = _get_job_info(params)) != SLURM_SUCCESS)
		return rc;
	if ((rc = _bcast_file(params)) != SLURM_SUCCESS)
		return rc;

/*	slurm_free_sbcast_cred_msg(sbcast_cred); */
	return rc;
}
