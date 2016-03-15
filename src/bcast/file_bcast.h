/****************************************************************************\
 *  file_bcast.h - definitions used for file broadcast functions
 *****************************************************************************
 *  Copyright (C) 2015-2016 SchedMD LLC.
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
\****************************************************************************/

#ifndef _FILE_BCAST_H
#define _FILE_BCAST_H

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include "slurm/slurm.h"
#include "src/common/macros.h"
#include "src/common/slurm_protocol_defs.h"

struct bcast_parameters {
	uint32_t block_size;
	uint16_t compress;
	int  fanout;
	bool force;
	uint32_t job_id;
	uint32_t step_id;
	bool preserve;
	int  timeout;
	int  verbose;
	char *src_fname;
	char *dst_fname;
};

extern int bcast_file(struct bcast_parameters *params);

#endif