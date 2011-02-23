/*****************************************************************************\
 *  block_allocator.c - Assorted functions for layout of bgq blocks,
 *	 wiring, mapping for smap, etc.
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2004-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2011 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "block_allocator.h"
#include "src/common/uid.h"
#include "src/common/timers.h"
#include "src/common/slurmdb_defs.h"

#define DEBUG_PA
#define BEST_COUNT_INIT 20

#define mp_strip_unaltered(__mp) (__mp & ~BA_MP_USED_ALTERED_PASS)

/* _ba_system is the "current" system that the structures will work
 *  on */
ba_mp_t ****ba_main_grid = NULL;
int best_count;
int color_count = 0;
uint16_t *deny_pass = NULL;
uint32_t ba_debug_flags = 0;
int DIM_SIZE[HIGHEST_DIMENSIONS] = {0,0,0,0};
static int REAL_DIM_SIZE[HIGHEST_DIMENSIONS] = {0,0,0,0};

typedef enum {
	BLOCK_ALGO_FIRST,
	BLOCK_ALGO_SECOND
} block_algo_t;

/** internal helper functions */
/* */
static void _rotate_geo(uint16_t *req_geo, int rot_cnt);

/* */
static int _check_for_options(select_ba_request_t* ba_request);

/* */
static int _append_geo(uint16_t *geo, List geos, int rotate);

/* */
static void _internal_removable_set_mps(int level, int *start,
					int *end, int *coords, bool mark);

/* */
static int _fill_in_coords(List results, int level, ba_mp_t *start_mp,
				    ba_mp_t **check_mp, uint16_t *block_start,
				    uint16_t *block_end, uint16_t *pass_end,
				    int *coords);

/* */
static char *_copy_from_main(List main_mps, List ret_list);

/* */
static char *_reset_altered_mps(List main_mps);

/* */
static int _copy_ba_switch(ba_mp_t *ba_mp, ba_mp_t *orig_mp, int dim);

/* */
static int _check_deny_pass(int dim);

/* */
static int _find_path(List mps, ba_mp_t *start_mp, int dim,
		      uint16_t geometry, uint16_t conn_type,
		      uint16_t *block_end, uint16_t *longest);

/* */
static int _setup_next_mps(ba_mp_t ****grid);

/* find the first block match in the system */
static int _find_match(select_ba_request_t* ba_request, List results);

/** */
static bool _mp_used(ba_mp_t* ba_mp, int dim);

/** */
static bool _mp_out_used(ba_mp_t* ba_mp, int dim);

/* */
/* static int _find_passthrough(ba_switch_t *curr_switch, int source_port,  */
/* 			     List mps, int dim,  */
/* 			     int count, int highest_phys_x);  */
/* */

/* */
/* static int _set_one_dim(uint16_t *start, uint16_t *end, uint16_t *coord); */

/* */
static void _destroy_geo(void *object);

/* */
static int _coord(char coord);

/*
 * create a block request.  Note that if the geometry is given,
 * then size is ignored.  If elongate is true, the algorithm will try
 * to fit that a block of cubic shape and then it will try other
 * elongated geometries.  (ie, 2x2x2 -> 4x2x1 -> 8x1x1).
 *
 * IN/OUT - ba_request: structure to allocate and fill in.
 *
 * ALL below IN's need to be set within the ba_request before the call
 * if you want them to be used.
 * ALL below OUT's are set and returned within the ba_request.
 * IN - avail_mp_bitmap: bitmap of usable midplanes.
 * IN - blrtsimage: BlrtsImage for this block if not default
 * IN - conn_type: connection type of request (TORUS or MESH or SMALL)
 * IN - elongate: if true, will try to fit different geometries of
 *      same size requests
 * IN/OUT - geometry: requested/returned geometry of block
 * IN - linuximage: LinuxImage for this block if not default
 * IN - mloaderimage: MLoaderImage for this block if not default
 * IN - nodecards: Number of nodecards in each block in request only
 *      used of small block allocations.
 * OUT - passthroughs: if there were passthroughs used in the
 *       generation of the block.
 * IN - procs: Number of real processors requested
 * IN - quarters: Number of midplane quarters in each block in request only
 *      used of small block allocations.
 * IN - RamDiskimage: RamDiskImage for this block if not default
 * IN - rotate: if true, allows rotation of block during fit
 * OUT - save_name: hostlist of midplanes used in block
 * IN/OUT - size: requested/returned count of midplanes in block
 * IN - start: geo location of where to start the allocation
 * IN - start_req: if set use the start variable to start at
 * return success of allocation/validation of params
 */
extern int new_ba_request(select_ba_request_t* ba_request)
{
	int i=0;
	float sz=1;
	int i2, picked, total_sz=1, size2=0;
	uint16_t *geo_ptr;
	int messed_with = 0;
	int checked[DIM_SIZE[A]];
	uint16_t geo[cluster_dims];

	/*FIXME: THis needs a good looking over for effencency in 4 dims. */

	memset(geo, 0, sizeof(geo));
	ba_request->save_name= NULL;
	ba_request->rotate_count= 0;
	ba_request->elongate_count = 0;
	ba_request->elongate_geos = list_create(_destroy_geo);
	memcpy(geo, ba_request->geometry, sizeof(geo));

	if (ba_request->deny_pass == (uint16_t)NO_VAL)
		ba_request->deny_pass = ba_deny_pass;

	if (!(cluster_flags & CLUSTER_FLAG_BGQ)) {
		if (geo[0] != (uint16_t)NO_VAL) {
			for (i=0; i<cluster_dims; i++) {
				if ((geo[i] < 1) || (geo[i] > DIM_SIZE[i])) {
					error("new_ba_request Error, "
					      "request geometry is invalid %d",
					      geo[i]);
					return 0;
				}
			}
			ba_request->size = ba_request->geometry[A];
		} else if (ba_request->size) {
			ba_request->geometry[A] = ba_request->size;
		} else
			return 0;
		return 1;
	}

	if (geo[0] != (uint16_t)NO_VAL) {
		for (i=0; i<cluster_dims; i++){
			if ((geo[i] < 1) || (geo[i] > DIM_SIZE[i])) {
				error("new_ba_request Error, "
				      "request geometry is invalid dim %d "
				      "can't be %c, largest is %c",
				      i,
				      alpha_num[geo[i]],
				      alpha_num[DIM_SIZE[i]]);
				return 0;
			}
		}
		_append_geo(geo, ba_request->elongate_geos, 0);
		sz=1;
		for (i=0; i<cluster_dims; i++)
			sz *= ba_request->geometry[i];
		ba_request->size = sz;
		sz=0;
	}

	deny_pass = &ba_request->deny_pass;

	if (ba_request->elongate || sz) {
		sz=1;
		/* decompose the size into a cubic geometry */
		ba_request->rotate= 1;
		ba_request->elongate = 1;

		for (i=0; i<cluster_dims; i++) {
			total_sz *= DIM_SIZE[i];
			geo[i] = 1;
		}

		if (ba_request->size == 1) {
			_append_geo(geo,
				    ba_request->elongate_geos,
				    ba_request->rotate);
			goto endit;
		}

		/* See if it can be placed in the Y dim, (Z is usually
		   the same size so rotate will get this if needed, so
		   only do it for 1 of the 2).
		*/
		if (ba_request->size <= DIM_SIZE[Y]) {
			geo[A] = 1;
			geo[X] = 1;
			geo[Y] = ba_request->size;
			geo[Z] = 1;
			sz=ba_request->size;
			_append_geo(geo,
				    ba_request->elongate_geos,
				    ba_request->rotate);
		}

		/* now see if you can set this in 1 plane. */
		i = ba_request->size/DIM_SIZE[Y];
		if (!(ba_request->size%2)
		    && i <= DIM_SIZE[Y]
		    && i <= DIM_SIZE[Z]
		    && i*i == ba_request->size) {
			geo[A] = 1;
			geo[X] = 1;
			geo[Y] = i;
			geo[Z] = i;
			sz=ba_request->size;
			_append_geo(geo,
				    ba_request->elongate_geos,
				    ba_request->rotate);
		}

		if (ba_request->size > total_sz || ba_request->size < 1)
			return 0;

		/* now see if this is a block that is a mutiple of a
		   plane. */
		sz = ba_request->size % (DIM_SIZE[Y] * DIM_SIZE[Z]);
		if (!sz) {
			i = ba_request->size / (DIM_SIZE[Y] * DIM_SIZE[Z]);
			if (i > DIM_SIZE[A]) {
				geo[A] = DIM_SIZE[A];
				geo[X] = i - DIM_SIZE[A];
			} else {
				geo[A] = i;
				geo[X] = 1;
			}
			geo[Y] = DIM_SIZE[Y];
			geo[Z] = DIM_SIZE[Z];
			sz=ba_request->size;
			if ((geo[A]*geo[X]*geo[Y]*geo[Z]) == ba_request->size)
				_append_geo(geo,
					    ba_request->elongate_geos,
					    ba_request->rotate);
			else
				error("%d I was just trying to add a "
				      "geo of %d%d%d%d "
				      "while I am trying to request "
				      "%d midplanes",
				      __LINE__, geo[A], geo[X], geo[Y], geo[Z],
				      ba_request->size);
		}
//	startagain:
		picked=0;
		for(i=0; i<DIM_SIZE[X]; i++)
			checked[i]=0;

		for (i=0; i<cluster_dims; i++) {
			total_sz *= DIM_SIZE[i];
			geo[i] = 1;
		}

		sz = 1;
		picked=0;
	tryagain:
		size2 = ba_request->size;
		//messedup:
		for (i=picked; i<cluster_dims; i++) {
			if (size2 <= 1)
				break;

			sz = size2 % DIM_SIZE[i];
			if (!sz) {
				geo[i] = DIM_SIZE[i];
				size2 /= DIM_SIZE[i];
			} else if (size2 > DIM_SIZE[i]) {
				for(i2=(DIM_SIZE[i]-1); i2 > 1; i2--) {
					/* go through each number to see if
					   the size is divisable by a smaller
					   number that is
					   good in the other dims. */
					if (!(size2%i2) && !checked[i2]) {
						size2 /= i2;

						if (i==0)
							checked[i2]=1;

						if (i2<DIM_SIZE[i]) {
							geo[i] = i2;
						} else {
							goto tryagain;
						}
						if ((i2-1)!=1 &&
						    i!=(cluster_dims-1))
							break;
					}
				}
				/* This size can not be made into a
				   block return.  If you want to try
				   until we find the next largest block
				   uncomment the code below and the goto
				   above. If a user specifies a max
				   mp count the job will never
				   run.
				*/
				if (i2==1) {
					if (!list_count(
						    ba_request->elongate_geos))
						error("Can't make a block of "
						      "%d into a cube.",
						      ba_request->size);
					goto endit;
/* 					ba_request->size +=1; */
/* 					goto startagain; */
				}
			} else {
				geo[i] = sz;
				break;
			}
		}

		if ((geo[X]*geo[Y]) <= DIM_SIZE[Y]) {
			ba_request->geometry[A] = geo[A];
			ba_request->geometry[X] = 1;
			ba_request->geometry[Y] = geo[X] * geo[Y];
			ba_request->geometry[Z] = geo[Z];
			_append_geo(ba_request->geometry,
				    ba_request->elongate_geos,
				    ba_request->rotate);

		}

		if ((geo[X]*geo[Z]) <= DIM_SIZE[Y]) {
			ba_request->geometry[A] = geo[A];
			ba_request->geometry[X] = 1;
			ba_request->geometry[Y] = geo[Y];
			ba_request->geometry[Z] = geo[X] * geo[Z];
			_append_geo(ba_request->geometry,
				    ba_request->elongate_geos,
				    ba_request->rotate);

		}

		/* Make sure geo[A] is even and then see if we can get
		   it into the Y or Z dim. */
		if (!(geo[A]%2) && ((geo[A]/2) <= DIM_SIZE[Y])) {
			ba_request->geometry[X] = geo[X];
			if (geo[Y] == 1) {
				ba_request->geometry[Y] = geo[A]/2;
				messed_with = 1;
			} else
				ba_request->geometry[Y] = geo[Y];
			if (!messed_with && geo[Z] == 1) {
				messed_with = 1;
				ba_request->geometry[Z] = geo[A]/2;
			} else
				ba_request->geometry[Z] = geo[Z];
			if (messed_with) {
				messed_with = 0;
				ba_request->geometry[A] = 2;
				_append_geo(ba_request->geometry,
					    ba_request->elongate_geos,
					    ba_request->rotate);
			}
		}
		if (geo[X] == DIM_SIZE[X] && (geo[Y] < DIM_SIZE[Y]
					      || geo[Z] < DIM_SIZE[Z])) {
			if (DIM_SIZE[Y]<DIM_SIZE[Z]) {
				i = DIM_SIZE[Y];
				DIM_SIZE[Y] = DIM_SIZE[Z];
				DIM_SIZE[Z] = i;
			}
			ba_request->geometry[A] = geo[A];
			ba_request->geometry[X] = geo[X];
			ba_request->geometry[Y] = geo[Y];
			ba_request->geometry[Z] = geo[Z];
			if (ba_request->geometry[Y] < DIM_SIZE[Y]) {
				i = (DIM_SIZE[Y] - ba_request->geometry[Y]);
				ba_request->geometry[Y] +=i;
			}
			if (ba_request->geometry[Z] < DIM_SIZE[Z]) {
				i = (DIM_SIZE[Z] - ba_request->geometry[Z]);
				ba_request->geometry[Z] +=i;
			}
			for(i = DIM_SIZE[X]; i>0; i--) {
				ba_request->geometry[X]--;
				i2 = (ba_request->geometry[X]
				      * ba_request->geometry[Y]
				      * ba_request->geometry[Z]);
				if (i2 < ba_request->size) {
					ba_request->geometry[X]++;
					messed_with = 1;
					break;
				}
			}
			if (messed_with) {
				messed_with = 0;
				_append_geo(ba_request->geometry,
					    ba_request->elongate_geos,
					    ba_request->rotate);
			}
		}

		if ((geo[A]*geo[X]*geo[Y]*geo[Z]) == ba_request->size)
			_append_geo(geo,
				    ba_request->elongate_geos,
				    ba_request->rotate);
		else
			error("%d I was just trying to add a geo of %d%d%d%d "
			      "while I am trying to request %d midplanes",
			      __LINE__, geo[A], geo[X], geo[Y], geo[Z],
			      ba_request->size);

/* Having the functions pow and powf on an aix system doesn't seem to
 * link well, so since this is only for aix and this doesn't really
 * need to be there just don't allow this extra calculation.
 */
#ifndef HAVE_AIX
		/* see if We can find a cube or square root of the
		   size to make an easy cube */
		for(i=0; i<cluster_dims-1; i++) {
			sz = powf((float)ba_request->size,
				  (float)1/(cluster_dims-i));
			if (pow(sz,(cluster_dims-i)) == ba_request->size)
				break;
		}

		if (i < (cluster_dims-1)) {
			/* we found something that looks like a cube! */
			int i3 = i;

			for (i=0; i<i3; i++)
				geo[i] = 1;

			for (i=i3; i<cluster_dims; i++)
				if (sz<=DIM_SIZE[i])
					geo[i] = sz;
				else
					goto endit;

			if ((geo[X]*geo[Y]*geo[Z]) == ba_request->size)
				_append_geo(geo,
					    ba_request->elongate_geos,
					    ba_request->rotate);
			else
				error("%d I was just trying to add "
				      "a geo of %d%d%d "
				      "while I am trying to request "
				      "%d midplanes",
				      __LINE__, geo[X], geo[Y], geo[Z],
				      ba_request->size);
		}
#endif //HAVE_AIX
	}

endit:
	/* reset */
	deny_pass = NULL;
	if (!(geo_ptr = (uint16_t *)list_peek(ba_request->elongate_geos)))
		return 0;

	ba_request->elongate_count++;
	ba_request->geometry[X] = geo_ptr[X];
	ba_request->geometry[Y] = geo_ptr[Y];
	ba_request->geometry[Z] = geo_ptr[Z];
	sz=1;
	for (i=0; i<cluster_dims; i++)
		sz *= ba_request->geometry[i];
	ba_request->size = sz;

	return 1;
}

/**
 * delete a block request
 */
extern void delete_ba_request(void *arg)
{
	select_ba_request_t *ba_request = (select_ba_request_t *)arg;
	if (ba_request) {
		xfree(ba_request->save_name);
		if (ba_request->elongate_geos)
			list_destroy(ba_request->elongate_geos);
		xfree(ba_request->mloaderimage);

		xfree(ba_request);
	}
}

/**
 * print a block request
 */
extern void print_ba_request(select_ba_request_t* ba_request)
{
	int i;

	if (ba_request == NULL){
		error("print_ba_request Error, request is NULL");
		return;
	}
	debug("  ba_request:");
	debug("    geometry:\t");
	for (i=0; i<cluster_dims; i++){
		debug("%d", ba_request->geometry[i]);
	}
	debug("        size:\t%d", ba_request->size);
	debug("   conn_type:\t%d", ba_request->conn_type[A]);
	debug("      rotate:\t%d", ba_request->rotate);
	debug("    elongate:\t%d", ba_request->elongate);
}

/**
 * empty a list that we don't want to destroy the memory of the
 * elements always returns 1
 */
extern int empty_null_destroy_list(void *arg, void *key)
{
	return 1;
}

extern void set_ba_debug_flags(uint32_t debug_flags)
{
	ba_debug_flags = debug_flags;
}

/*
 * set the mp in the internal configuration as in, or not in use,
 * along with the current state of the mp.
 *
 * IN ba_mp: ba_mp_t to update state
 * IN state: new state of ba_mp_t
 */
extern void ba_update_mp_state(ba_mp_t *ba_mp, uint16_t state)
{
	uint16_t mp_base_state = state & NODE_STATE_BASE;
	uint16_t mp_flags = state & NODE_STATE_FLAGS;

	if (!ba_initialized){
		error("Error, configuration not initialized, "
		      "calling ba_init(NULL, 1)");
		ba_init(NULL, 1);
	}

#ifdef HAVE_BGQ
	debug2("ba_update_mp_state: new state of [%s] is %s",
	       ba_mp->coord_str, node_state_string(state));
#else
	debug2("ba_update_mp_state: new state of [%d] is %s",
	       ba_mp->coord[A],
	       node_state_string(state));
#endif

	/* basically set the mp as used */
	if ((mp_base_state == NODE_STATE_DOWN)
	    || (mp_flags & (NODE_STATE_DRAIN | NODE_STATE_FAIL)))
		ba_mp->used = BA_MP_USED_TRUE;
	else
		ba_mp->used = BA_MP_USED_FALSE;

	ba_mp->state = state;
}

extern ba_mp_t *coord2ba_mp(int *coord)
{
	return &ba_main_grid[coord[A]][coord[X]][coord[Y]][coord[Z]];
}


/*
 * Try to allocate a block.
 *
 * IN - ba_request: allocation request
 * OUT - results: List of results of the allocation request.  Each
 * list entry will be a coordinate.  allocate_block will create the
 * list, but the caller must destroy it.
 *
 * return: success or error of request
 */
extern int allocate_block(select_ba_request_t* ba_request, List results)
{
	if (!ba_initialized){
		error("Error, configuration not initialized, "
		      "calling ba_init(NULL, 1)");
		ba_init(NULL, 1);
	}

	if (!ba_request){
		error("allocate_block Error, request not initialized");
		return 0;
	}

	// _backup_ba_system();
	if (_find_match(ba_request, results)){
		return 1;
	} else {
		return 0;
	}
}


/*
 * Admin wants to remove a previous allocation.
 * will allow Admin to delete a previous allocation retrival by letter code.
 */
extern int remove_block(List mps, int new_count, bool is_small)
{
	int dim;
	ba_mp_t* curr_ba_mp = NULL;
	ba_mp_t* ba_mp = NULL;
	ListIterator itr;

	itr = list_iterator_create(mps);
	while ((curr_ba_mp = (ba_mp_t*) list_next(itr))) {
		/* since the list that comes in might not be pointers
		   to the main list we need to point to that main list */
		ba_mp = &ba_main_grid
			[curr_ba_mp->coord[A]]
			[curr_ba_mp->coord[X]]
			[curr_ba_mp->coord[Y]]
			[curr_ba_mp->coord[Z]];

		ba_mp->used = false;
		/* Small blocks don't use wires, and only have 1 mp,
		   so just break. */
		if (is_small)
			break;
		for(dim=0; dim<cluster_dims; dim++) {
			if (curr_ba_mp == ba_mp) {
				/* Remove the usage that was altered */
				ba_mp->axis_switch[dim].usage &=
					(~ba_mp->alter_switch[dim].usage);
				ba_mp->alter_switch[dim].usage =
					BG_SWITCH_NONE;
			} else {
				/* Just remove the usage set here */
				ba_mp->axis_switch[dim].usage &=
					(~curr_ba_mp->axis_switch[dim].usage);
			}
		}
	}
	list_iterator_destroy(itr);
	if (new_count == (int)NO_VAL) {
	} else if (new_count == -1)
		color_count--;
	else
		color_count=new_count;
	if (color_count < 0)
		color_count = 0;
	return 1;
}

/*
 * Used to set a block into a virtual system.  The system can be
 * cleared first and this function sets all the wires and midplanes
 * used in the mplist given.  The mplist is a list of ba_mp_t's
 * that are already set up.  This is very handly to test if there are
 * any passthroughs used by one block when adding another block that
 * also uses those wires, and neither use any overlapping
 * midplanes. Doing a simple bitmap & will not reveal this.
 *
 * Returns SLURM_SUCCESS if mplist fits into system without
 * conflict, and SLURM_ERROR if mplist conflicts with something
 * already in the system.
 */
extern int check_and_set_mp_list(List mps)
{
	int rc = SLURM_ERROR;

#ifdef HAVE_BGQ
	int i;
	ba_switch_t *ba_switch = NULL, *curr_ba_switch = NULL;
	ba_mp_t *ba_mp = NULL, *curr_ba_mp = NULL;
	ListIterator itr = NULL;

	if (!mps)
		return rc;

	itr = list_iterator_create(mps);
	while ((ba_mp = list_next(itr))) {
		/* info("checking %c%c%c", */
/* 		     ba_mp->coord[X],  */
/* 		     ba_mp->coord[Y], */
/* 		     ba_mp->coord[Z]); */

		curr_ba_mp = &ba_main_grid
			[ba_mp->coord[A]]
			[ba_mp->coord[X]]
			[ba_mp->coord[Y]]
			[ba_mp->coord[Z]];

		if (ba_mp->used && curr_ba_mp->used) {
			/* Only error if the midplane isn't already
			 * marked down or in a error state outside of
			 * the bluegene block.
			 */
			uint16_t base_state, mp_flags;
			base_state = curr_ba_mp->state & NODE_STATE_BASE;
			mp_flags = curr_ba_mp->state & NODE_STATE_FLAGS;
			if (!(mp_flags & (NODE_STATE_DRAIN | NODE_STATE_FAIL))
			    && (base_state != NODE_STATE_DOWN)) {
				if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
					info("check_and_set_mp_list: "
					     "I have already been to "
					     "this mp %s %s",
					     ba_mp->coord_str,
					     node_state_string(
						     curr_ba_mp->state));
				rc = SLURM_ERROR;
				goto end_it;
			}
		}

		if (ba_mp->used)
			curr_ba_mp->used = BA_MP_USED_TRUE;
		for(i=0; i<cluster_dims; i++) {
			ba_switch = &ba_mp->axis_switch[i];
			curr_ba_switch = &curr_ba_mp->axis_switch[i];
			//info("checking dim %d", i);

			if (ba_switch->usage == BG_SWITCH_NONE)
				continue;

			if (bg_switch_overlap(ba_switch->usage,
					      curr_ba_switch->usage)) {
				if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
					info("check_and_set_mp_list: "
					     "%s dim %d is already in "
					     "use the way we want to use it."
					     "%s already at %s",
					     ba_mp->coord_str, i,
					     ba_switch_usage_str(
						     ba_switch->usage),
					     ba_switch_usage_str(
						     curr_ba_switch->usage));
				rc = SLURM_ERROR;
				goto end_it;
			}

			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("check_and_set_mp_list: "
				     "setting %s dim %d to from %s to %s",
				     ba_mp->coord_str, i,
				     ba_switch_usage_str(curr_ba_switch->usage),
				     ba_switch_usage_str(curr_ba_switch->usage
							 | ba_switch->usage));
			curr_ba_switch->usage |= ba_switch->usage;
		}
	}
	rc = SLURM_SUCCESS;
end_it:
	list_iterator_destroy(itr);
#endif
	return rc;
}

/*
 * Used to find, and set up midplanes and the wires in the virtual
 * system and return them in List results
 *
 * IN/OUT results - a list with a NULL destroyer filled in with
 *        midplanes and wires set to create the block with the api. If
 *        only interested in the hostlist NULL can be excepted also.
 * IN start - where to start the allocation.
 * IN geometry - the requested geometry of the block.
 * IN conn_type - mesh, torus, or small.
 *
 * RET char * - hostlist of midplanes results represent must be
 *     xfreed.  NULL on failure
 */
extern char *set_bg_block(List results, uint16_t *start,
			  uint16_t *geometry, uint16_t *conn_type)
{
	List main_mps = NULL;
	char *name = NULL;
	ba_mp_t* ba_mp = NULL;
	ba_mp_t *check_mp[cluster_dims];
	int size = 1, dim;
	uint16_t block_end[cluster_dims];
	uint16_t pass_end[cluster_dims];
	int coords[cluster_dims];

	if (!ba_initialized){
		error("Error, configuration not initialized, "
		      "calling ba_init(NULL, 1)");
		ba_init(NULL, 1);
	}

	if (cluster_dims == 1) {
		if (start[A] >= DIM_SIZE[A])
			return NULL;
		size = geometry[X];
		ba_mp = &ba_main_grid[start[A]][0][0][0];
	} else {
		for (dim=0; dim<cluster_dims; dim++) {
			if (start[dim] >= DIM_SIZE[dim])
				return NULL;
			if (geometry[dim] <= 0) {
				error("problem with geometry of %c in dim %d, "
				      "needs to be at least 1",
				      alpha_num[geometry[dim]], dim);
				return NULL;
			}
			size *= geometry[dim];
		}

		ba_mp = &ba_main_grid[start[A]][start[X]][start[Y]][start[Z]];
		/* info("looking at %s", ba_mp->coord_str); */
	}

	if (!ba_mp)
		return NULL;

	if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
		info("trying mp %s %c%c%c%c %d",
		     ba_mp->coord_str,
		     alpha_num[geometry[A]],
		     alpha_num[geometry[X]],
		     alpha_num[geometry[Y]],
		     alpha_num[geometry[Z]],
		     conn_type[A]);

	if (conn_type[A] >= SELECT_SMALL) {
		/* adding the ba_mp and ending */
		if (results)
			list_append(results, ba_mp);

		ba_mp->used = BA_MP_USED_TRUE;
		name = xstrdup(ba_mp->coord_str);
		goto end_it;
	}

	main_mps = list_create(NULL);
	/* This midplane should have already been checked if it was in
	   use or not */
	list_append(main_mps, ba_mp);

	/* set the end to the start and the _find_path will increase each dim.*/
	memcpy(block_end, start, sizeof(block_end));
	memcpy(pass_end, start, sizeof(pass_end));
	for (dim=0; dim<cluster_dims; dim++) {
		info("got mp of %p", ba_mp);
		if (!_find_path(main_mps, ba_mp, dim,
				geometry[dim], conn_type[dim], &block_end[dim],
				&pass_end[dim])) {
			goto end_it;
		}
	}
	if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
		info("complete box is %c%c%c%c x %c%c%c%c pass to %c%c%c%c",
		     alpha_num[start[A]],
		     alpha_num[start[X]],
		     alpha_num[start[Y]],
		     alpha_num[start[Z]],
		     alpha_num[block_end[A]],
		     alpha_num[block_end[X]],
		     alpha_num[block_end[Y]],
		     alpha_num[block_end[Z]],
		     alpha_num[pass_end[A]],
		     alpha_num[pass_end[X]],
		     alpha_num[pass_end[Y]],
		     alpha_num[pass_end[Z]]);

	if (_fill_in_coords(main_mps, A, ba_mp, check_mp,
			    start, block_end, pass_end, coords) == -1)
		goto end_it;

	/* Success */
	if (results)
		name = _copy_from_main(main_mps, results);
	else
		name = _reset_altered_mps(main_mps);

end_it:

	if (main_mps) {
		list_destroy(main_mps);
		main_mps = NULL;
	}
	if (name!=NULL) {
		debug2("name = %s", name);
	} else {
		debug2("can't allocate");
		xfree(name);
	}

	return name;
}

/*
 * Resets the virtual system to a virgin state.  If track_down_mps is set
 * then those midplanes are not set to idle, but kept in a down state.
 */
extern void reset_ba_system(bool track_down_mps)
{
	int a, x, y, z;

	for (a = 0; a < DIM_SIZE[A]; a++)
		for (x = 0; x < DIM_SIZE[X]; x++)
			for (y = 0; y < DIM_SIZE[Y]; y++)
				for (z = 0; z < DIM_SIZE[Z]; z++) {
					ba_mp_t *ba_mp = &ba_main_grid
						[a][x][y][z];
					ba_setup_mp(ba_mp, track_down_mps);
				}
}

/*
 * Used to set all midplanes in a special used state except the ones
 * we are able to use in a new allocation.
 *
 * IN: hostlist of midplanes we do not want
 * RET: SLURM_SUCCESS on success, or SLURM_ERROR on error
 *
 * Note: Need to call reset_all_removed_mps before starting another
 * allocation attempt after
 */
extern int removable_set_mps(char *mps)
{
#ifdef HAVE_BGQ
	int j=0, number;
	int a,x;
	int y,z;
	int start[cluster_dims];
        int end[cluster_dims];
	int coords[cluster_dims];
	char *p = '\0';

	if (!mps)
		return SLURM_ERROR;

	while (mps[j] != '\0') {
		if ((mps[j] == '[' || mps[j] == ',')
		    && (mps[j+10] == ']' || mps[j+8] == ',')
		    && (mps[j+5] == 'x' || mps[j+4] == '-')) {

			j++;
			number = xstrntol(mps + j, &p, cluster_dims,
					  cluster_base);
			hostlist_parse_int_to_array(
				number, start, cluster_dims, cluster_base);
			j += 4;
			number = xstrntol(mps + j, &p, cluster_dims,
					  cluster_base);
			hostlist_parse_int_to_array(
				number, end, cluster_dims, cluster_base);
			j += 3;

			_internal_removable_set_mps(A, start, end, coords, 1);

			if (mps[j] != ',')
				break;
			j--;
		} else if ((mps[j] >= '0' && mps[j] <= '9')
			   || (mps[j] >= 'A' && mps[j] <= 'D')) {
			number = xstrntol(mps + j, &p, cluster_dims,
					  cluster_base);
			hostlist_parse_int_to_array(
				number, start, cluster_dims, cluster_base);
			a = start[A];
			x = start[X];
			y = start[Y];
			z = start[Z];
			j+=3;
			if (ba_main_grid[a][x][y][z].used == BA_MP_USED_FALSE)
				ba_main_grid[a][x][y][z].used = BA_MP_USED_TEMP;

			if (mps[j] != ',')
				break;
			j--;
		}
		j++;
	}
#endif
 	return SLURM_SUCCESS;
}

/*
 * Resets the virtual system to the pervious state before calling
 * removable_set_mps, or set_all_mps_except.
 */
extern int reset_all_removed_mps()
{
	int start[cluster_dims];
  	int coords[cluster_dims];

	memset(start, 0, sizeof(start));
	_internal_removable_set_mps(A, start, DIM_SIZE, coords, 0);

	return SLURM_SUCCESS;
}

/*
 * IN: hostlist of midplanes we want to be able to use, mark all
 *     others as used.
 * RET: SLURM_SUCCESS on success, or SLURM_ERROR on error
 *
 * Need to call reset_all_removed_mps before starting another
 * allocation attempt if possible use removable_set_mps since it is
 * faster. It does basically the opposite of this function. If you
 * have to come up with this list though it is faster to use this
 * function than if you have to call bitmap2node_name since that is slow.
 */
extern int set_all_mps_except(char *mps)
{
	int a, x, y, z;
	hostlist_t hl = hostlist_create(mps);
	char *host = NULL, *numeric = NULL;
	int number, coords[HIGHEST_DIMENSIONS];
	char *p = '\0';

	memset(coords, 0, sizeof(coords));

	while ((host = hostlist_shift(hl))){
		numeric = host;
		number = 0;
		while (numeric) {
			if (numeric[0] < '0' || numeric[0] > 'D'
			    || (numeric[0] > '9'
				&& numeric[0] < 'A')) {
				numeric++;
				continue;
			}
			number = xstrntol(numeric, &p, cluster_dims,
					  cluster_base);
			break;
		}
		hostlist_parse_int_to_array(
			number, coords, cluster_dims, cluster_base);
		ba_main_grid[coords[A]][coords[X]][coords[Y]][coords[Z]].state
			|= NODE_RESUME;
		free(host);
	}
	hostlist_destroy(hl);

	for (a = 0; a < DIM_SIZE[A]; a++)
		for (x = 0; x < DIM_SIZE[X]; x++)
			for (y = 0; y < DIM_SIZE[Y]; y++)
				for (z = 0; z < DIM_SIZE[Z]; z++) {
					if (ba_main_grid[a][x][y][z].state
					    & NODE_RESUME) {
						/* clear the bit and
						 * mark as unused */
						ba_main_grid[a][x][y][z].state
							&= ~NODE_RESUME;
					} else if (!ba_main_grid
						   [a][x][y][z].used) {
						ba_main_grid[a][x][y][z].used
							= BA_MP_USED_TEMP;
					}
				}

 	return SLURM_SUCCESS;
}

/*
 * set values of every grid point (used in smap)
 */
extern void init_grid(node_info_msg_t * node_info_ptr)
{
	int i = 0, j, a, x, y, z;
	ba_mp_t *ba_mp = NULL;

	if (!node_info_ptr) {
		for (a = 0; a < DIM_SIZE[A]; a++) {
			for (x = 0; x < DIM_SIZE[X]; x++) {
				for (y = 0; y < DIM_SIZE[Y]; y++) {
					for (z = 0; z < DIM_SIZE[Z]; z++) {
						ba_mp = &ba_main_grid
							[a][x][y][z];
						ba_mp->state = NODE_STATE_IDLE;
						ba_mp->index = i++;
					}
				}
			}
		}
		return;
	}

	for (j = 0; j < (int)node_info_ptr->record_count; j++) {
		int coord[cluster_dims];
		node_info_t *node_ptr = &node_info_ptr->node_array[j];
		if (!node_ptr->name)
			continue;

		memset(coord, 0, sizeof(coord));
		if (cluster_dims == 1) {
			coord[0] = j;
		} else {
			if ((i = strlen(node_ptr->name)) < 4)
				continue;
			for (x=0; x<cluster_dims; x++)
				coord[x] = _coord(node_ptr->name[i-(4+x)]);
		}

		for (x=0; x<cluster_dims; x++)
			if (coord[x] < 0)
				break;
		if (x < cluster_dims)
			continue;

		ba_mp = &ba_main_grid[coord[A]][coord[X]][coord[Y]][coord[Z]];
		ba_mp->index = j;
		ba_mp->state = node_ptr->node_state;
	}
}

/*
 * find a base blocks bg location
 */
extern uint16_t *find_mp_loc(char* mp_id)
{
	char *check = NULL;
	uint32_t a, x, y, z;
	ba_mp_t *ba_mp = NULL;

	bridge_setup_system();

	check = xstrdup(mp_id);
	/* with BGP they changed the names of the rack midplane action from
	 * R000 to R00-M0 so we now support both formats for each of the
	 * systems */
#ifdef HAVE_BGL
	if (check[3] == '-') {
		if (check[5]) {
			check[3] = check[5];
			check[4] = '\0';
		}
	}

	if ((check[1] < '0' || check[1] > '9')
	    || (check[2] < '0' || check[2] > '9')
	    || (check[3] < '0' || check[3] > '9')) {
		error("%s is not a valid Rack-Midplane (i.e. R000)", mp_id);
		goto cleanup;
	}

#else
	if (check[3] != '-') {
		xfree(check);
		check = xstrdup_printf("R%c%c-M%c",
				       mp_id[1], mp_id[2], mp_id[3]);
	}

	if ((check[1] < '0' || check[1] > '9')
	    || (check[2] < '0' || check[2] > '9')
	    || (check[5] < '0' || check[5] > '9')) {
		error("%s is not a valid Rack-Midplane (i.e. R00-M0)", mp_id);
		goto cleanup;
	}
#endif

	for (a = 0; a <= DIM_SIZE[A]; a++)
		for (x = 0; x <= DIM_SIZE[X]; x++)
			for (y = 0; y <= DIM_SIZE[Y]; y++)
				for (z = 0; z <= DIM_SIZE[Z]; z++)
					if (!strcasecmp(ba_main_grid
							[a][x][y][z].loc,
							check)) {
						ba_mp = &ba_main_grid
							[a][x][y][z];
						goto cleanup; /* we found it */
					}

cleanup:
	xfree(check);

	if (ba_mp != NULL)
		return ba_mp->coord;
	else
		return NULL;
}

/*
 * find a rack/midplace location
 */
extern char *find_mp_rack_mid(char* coords)
{
	ba_mp_t *curr_mp;

	if(!(curr_mp = str2ba_mp(coords)))
		return NULL;

	bridge_setup_system();

	return curr_mp->loc;
}


/*
 * get the used wires for a block out of the database and return the
 * node list.  The block_ptr here must be gotten with bridge_get_block
 * not bridge_get_block_info, if you are looking to recover from
 * before.  If you are looking to start clean it doesn't matter.
 */
extern List get_and_set_block_wiring(char *bg_block_id,
				     void *block_ptr)
{
#if 0
	int rc, i, j;
	int cnt = 0;
	int switch_cnt = 0;
	rm_switch_t *curr_switch = NULL;
	rm_BP_t *curr_mp = NULL;
	char *switchid = NULL;
	rm_connection_t curr_conn;
	int dim;
	ba_mp_t *ba_mp = NULL;
	ba_switch_t *ba_switch = NULL;
	uint16_t *geo = NULL;
	List results = list_create(destroy_ba_mp);
	ListIterator itr = NULL;

	if (ba_debug_flags & DEBUG_FLAG_BG_ALGO)
		info("getting info for block %s", bg_block_id);

	if ((rc = bridge_get_data(block_ptr, RM_PartitionSwitchNum,
				  &switch_cnt)) != STATUS_OK) {
		error("bridge_get_data(RM_PartitionSwitchNum): %s",
		      bridge_err_str(rc));
		goto end_it;
	}
	if (!switch_cnt) {
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
			info("no switch_cnt");
		if ((rc = bridge_get_data(block_ptr,
					  RM_PartitionFirstBP,
					  &curr_mp))
		    != STATUS_OK) {
			error("bridge_get_data: "
			      "RM_PartitionFirstBP: %s",
			      bridge_err_str(rc));
			goto end_it;
		}
		if ((rc = bridge_get_data(curr_mp, RM_BPID, &switchid))
		    != STATUS_OK) {
			error("bridge_get_data: RM_SwitchBPID: %s",
			      bridge_err_str(rc));
			goto end_it;
		}

		geo = find_mp_loc(switchid);
		if (!geo) {
			error("find_mp_loc: mpid %s not known", switchid);
			goto end_it;
		}
		ba_mp = xmalloc(sizeof(ba_mp_t));
		list_push(results, ba_mp);
		ba_mp->coord[X] = geo[X];
		ba_mp->coord[Y] = geo[Y];
		ba_mp->coord[Z] = geo[Z];

		ba_mp->used = BA_MP_USED_TRUE;
		return results;
	}
	for (i=0; i<switch_cnt; i++) {
		if (i) {
			if ((rc = bridge_get_data(block_ptr,
						  RM_PartitionNextSwitch,
						  &curr_switch))
			    != STATUS_OK) {
				error("bridge_get_data: "
				      "RM_PartitionNextSwitch: %s",
				      bridge_err_str(rc));
				goto end_it;
			}
		} else {
			if ((rc = bridge_get_data(block_ptr,
						  RM_PartitionFirstSwitch,
						  &curr_switch))
			    != STATUS_OK) {
				error("bridge_get_data: "
				      "RM_PartitionFirstSwitch: %s",
				      bridge_err_str(rc));
				goto end_it;
			}
		}
		if ((rc = bridge_get_data(curr_switch, RM_SwitchDim, &dim))
		    != STATUS_OK) {
			error("bridge_get_data: RM_SwitchDim: %s",
			      bridge_err_str(rc));
			goto end_it;
		}
		if ((rc = bridge_get_data(curr_switch, RM_SwitchBPID,
					  &switchid))
		    != STATUS_OK) {
			error("bridge_get_data: RM_SwitchBPID: %s",
			      bridge_err_str(rc));
			goto end_it;
		}

		geo = find_mp_loc(switchid);
		if (!geo) {
			error("find_mp_loc: mpid %s not known", switchid);
			goto end_it;
		}

		if ((rc = bridge_get_data(curr_switch, RM_SwitchConnNum, &cnt))
		    != STATUS_OK) {
			error("bridge_get_data: RM_SwitchBPID: %s",
			      bridge_err_str(rc));
			goto end_it;
		}
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO)
			info("switch id = %s dim %d conns = %d",
			     switchid, dim, cnt);

		itr = list_iterator_create(results);
		while ((ba_mp = list_next(itr))) {
			if (ba_mp->coord[X] == geo[X] &&
			    ba_mp->coord[Y] == geo[Y] &&
			    ba_mp->coord[Z] == geo[Z])
				break;	/* we found it */
		}
		list_iterator_destroy(itr);
		if (!ba_mp) {
			ba_mp = xmalloc(sizeof(ba_mp_t));

			list_push(results, ba_mp);
			ba_mp->coord[X] = geo[X];
			ba_mp->coord[Y] = geo[Y];
			ba_mp->coord[Z] = geo[Z];
		}
		ba_switch = &ba_mp->axis_switch[dim];
		for (j=0; j<cnt; j++) {
			if (j) {
				if ((rc = bridge_get_data(
					     curr_switch,
					     RM_SwitchNextConnection,
					     &curr_conn))
				    != STATUS_OK) {
					error("bridge_get_data: "
					      "RM_SwitchNextConnection: %s",
					      bridge_err_str(rc));
					goto end_it;
				}
			} else {
				if ((rc = bridge_get_data(
					     curr_switch,
					     RM_SwitchFirstConnection,
					     &curr_conn))
				    != STATUS_OK) {
					error("bridge_get_data: "
					      "RM_SwitchFirstConnection: %s",
					      bridge_err_str(rc));
					goto end_it;
				}
			}
			switch(curr_conn.p1) {
			case RM_PORT_S1:
				curr_conn.p1 = 1;
				break;
			case RM_PORT_S2:
				curr_conn.p1 = 2;
				break;
			case RM_PORT_S4:
				curr_conn.p1 = 4;
				break;
			default:
				error("1 unknown port %d",
				      _port_enum(curr_conn.p1));
				goto end_it;
			}

			switch(curr_conn.p2) {
			case RM_PORT_S0:
				curr_conn.p2 = 0;
				break;
			case RM_PORT_S3:
				curr_conn.p2 = 3;
				break;
			case RM_PORT_S5:
				curr_conn.p2 = 5;
				break;
			default:
				error("2 unknown port %d",
				      _port_enum(curr_conn.p2));
				goto end_it;
			}

			if (curr_conn.p1 == 1 && dim == B) {
				if (ba_mp->used) {
					debug("I have already been to "
					      "this node %c%c%c%c",
					      alpha_num[geo[A]],
					      alpha_num[geo[X]],
					      alpha_num[geo[Y]],
					      alpha_num[geo[Z]]);
					goto end_it;
				}
				ba_mp->used = BA_MP_USED_TRUE;
			}
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("connection going from %d -> %d",
				     curr_conn.p1, curr_conn.p2);

			if (ba_switch->int_wire[curr_conn.p1].used) {
				debug("%c%c%c%c dim %d port %d "
				      "is already in use",
				      alpha_num[geo[A]],
				      alpha_num[geo[X]],
				      alpha_num[geo[Y]],
				      alpha_num[geo[Z]],
				      dim,
				      curr_conn.p1);
				goto end_it;
			}
			ba_switch->int_wire[curr_conn.p1].used = 1;
			ba_switch->int_wire[curr_conn.p1].port_tar
				= curr_conn.p2;

			if (ba_switch->int_wire[curr_conn.p2].used) {
				debug("%c%c%c%c dim %d port %d "
				      "is already in use",
				      alpha_num[geo[A]],
				      alpha_num[geo[X]],
				      alpha_num[geo[Y]],
				      alpha_num[geo[Z]],
				      dim,
				      curr_conn.p2);
				goto end_it;
			}
			ba_switch->int_wire[curr_conn.p2].used = 1;
			ba_switch->int_wire[curr_conn.p2].port_tar
				= curr_conn.p1;
		}
	}
	return results;
end_it:
	list_destroy(results);
	return NULL;
#else
	return NULL;
#endif

}

/* */
extern int validate_coord(uint16_t *coord)
{
	int dim;

	for (dim=0; dim < cluster_dims; dim++) {
		if (coord[dim] >= REAL_DIM_SIZE[dim]) {
			error("got coord %c%c%c%c greater than system dims "
			      "%c%c%c%c",
			      alpha_num[coord[A]],
			      alpha_num[coord[X]],
			      alpha_num[coord[Y]],
			      alpha_num[coord[Z]],
			      alpha_num[REAL_DIM_SIZE[A]],
			      alpha_num[REAL_DIM_SIZE[X]],
			      alpha_num[REAL_DIM_SIZE[Y]],
			      alpha_num[REAL_DIM_SIZE[Z]]);
			return 0;
		} else if (coord[dim] >= DIM_SIZE[dim]) {
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("got coord %c%c%c%c greater than what "
				     "we are using %c%c%c%c",
				     alpha_num[coord[A]],
				     alpha_num[coord[X]],
				     alpha_num[coord[Y]],
				     alpha_num[coord[Z]],
				     alpha_num[DIM_SIZE[A]],
				     alpha_num[DIM_SIZE[X]],
				     alpha_num[DIM_SIZE[Y]],
				     alpha_num[DIM_SIZE[Z]]);
			return 0;
		}
	}

	return 1;
}

extern char *ba_switch_usage_str(uint16_t usage)
{
	switch (usage) {
	case BG_SWITCH_NONE:
		return "None";
	case BG_SWITCH_WRAPPED_PASS:
		return "WrappedPass";
	case BG_SWITCH_TORUS:
		return "FullTorus";
	case BG_SWITCH_PASS:
		return "Passthrough";
	case BG_SWITCH_WRAPPED:
		return "Wrapped";
	case (BG_SWITCH_OUT | BG_SWITCH_OUT_PASS):
		return "OutLeaving";
	case BG_SWITCH_OUT:
		return "Out";
	case (BG_SWITCH_IN | BG_SWITCH_IN_PASS):
		return "InComming";
	case BG_SWITCH_IN:
		return "In";
	default:
		error("unknown switch usage %u", usage);
		break;
	}
	return "unknown";
}

extern bool ba_rotate_geo(uint16_t *match_geo, uint16_t *req_geo)
{
	bool match = false;
	int rot_cnt = 0;	/* attempt 12 rotations  */
	int dim = 0;

	for (rot_cnt=0; rot_cnt<12; rot_cnt++) {
		for (dim = 0; dim < cluster_dims; dim++) {
			if (match_geo[dim] < req_geo[dim])
				break;
		}

		if (dim >= cluster_dims) {
			match = true;
			break;
		}

		_rotate_geo(req_geo, rot_cnt);
	}

	return match;
}

static void _rotate_geo(uint16_t *req_geo, int rot_cnt)
{
	uint16_t tmp;

	switch (rot_cnt) {
	case 0:		/* ABCD -> ABDC */
	case 3:		/* DABC -> DACB */
	case 6:		/* CDAB -> CDBA */
	case 9:		/* BCDA -> BCAD */
		SWAP(req_geo[Y], req_geo[Z], tmp);
		break;
	case 1:		/* ABDC -> ADBC */
	case 4:		/* DACB -> DCAB */
	case 7:		/* CDBA -> CBDA */
	case 10:	/* BCAD -> BACD */
		SWAP(req_geo[X], req_geo[Y], tmp);
		break;
	case 2:		/* ABDC -> DABC */
	case 5:		/* DCAB -> CDAB */
	case 8:		/* CBDA -> BCDA */
	case 11:	/* BACD -> ABCD */
		SWAP(req_geo[A], req_geo[X], tmp);
		break;
	}
}

/*
 * This function is here to check options for rotating and elongating
 * and set up the request based on the count of each option
 */
static int _check_for_options(select_ba_request_t* ba_request)
{
	int temp;
	int set=0;
	uint16_t *geo = NULL;
	ListIterator itr;

	if (ba_request->rotate) {
	rotate_again:
		debug2("Rotating! %d",ba_request->rotate_count);

		if (ba_request->rotate_count==(cluster_dims-1)) {
			temp=ba_request->geometry[A];
			ba_request->geometry[A] = ba_request->geometry[Z];
			ba_request->geometry[Z] = temp;
			ba_request->rotate_count++;
			set=1;

		} else if (ba_request->rotate_count<(cluster_dims*2)) {
			temp=ba_request->geometry[X];
			ba_request->geometry[A] = ba_request->geometry[X];
			ba_request->geometry[X] = ba_request->geometry[Y];
			ba_request->geometry[Y] = ba_request->geometry[Z];
			ba_request->geometry[Z] = temp;
			ba_request->rotate_count++;
			set=1;
		} else
			ba_request->rotate = false;
		if (set) {
			int i;
			for (i = 0; i < cluster_dims; i++)
				if (ba_request->geometry[i] > DIM_SIZE[A]) {
					set = 0;
					goto rotate_again;
				}
			return 1;
		}
	}
	if (ba_request->elongate) {
	elongate_again:
		debug2("Elongating! %d",ba_request->elongate_count);
		ba_request->rotate_count=0;
		ba_request->rotate = true;

		set = 0;
		itr = list_iterator_create(ba_request->elongate_geos);
		for(set=0; set<=ba_request->elongate_count; set++)
			geo = (uint16_t *)list_next(itr);
		list_iterator_destroy(itr);
		if (geo == NULL)
			return 0;
		ba_request->elongate_count++;
		ba_request->geometry[A] = geo[A];
		ba_request->geometry[X] = geo[X];
		ba_request->geometry[Y] = geo[Y];
		ba_request->geometry[Z] = geo[Z];
		if (ba_request->geometry[A]<=DIM_SIZE[A]
		    && ba_request->geometry[X]<=DIM_SIZE[X]
		    && ba_request->geometry[Y]<=DIM_SIZE[Y]
		    && ba_request->geometry[Z]<=DIM_SIZE[Z]) {
			return 1;
		} else
			goto elongate_again;

	}
	return 0;
}

/*
 * grab all the geometries that we can get and append them to the list geos
 */
static int _append_geo(uint16_t *geometry, List geos, int rotate)
{
	ListIterator itr;
	uint16_t *geo_ptr = NULL;
	uint16_t *geo = NULL;
	int temp_geo;
	int i, j;

	if (rotate) {
		for (i = (cluster_dims - 1); i >= 0; i--) {
			for (j = 1; j <= i; j++) {
				if ((geometry[j-1] > geometry[j])
				    && (geometry[j] <= DIM_SIZE[j-i])
				    && (geometry[j-1] <= DIM_SIZE[j])) {
					temp_geo = geometry[j-1];
					geometry[j-1] = geometry[j];
					geometry[j] = temp_geo;
				}
			}
		}
	}
	itr = list_iterator_create(geos);
	while ((geo_ptr = (uint16_t *)list_next(itr)) != NULL) {
		if (geometry[A] == geo_ptr[A]
		    && geometry[X] == geo_ptr[X]
		    && geometry[Y] == geo_ptr[Y]
		    && geometry[Z] == geo_ptr[Z])
			break;

	}
	list_iterator_destroy(itr);

	if (geo_ptr == NULL) {
		geo = (uint16_t *)xmalloc(sizeof(uint16_t)*cluster_dims);
		geo[A] = geometry[A];
		geo[X] = geometry[X];
		geo[Y] = geometry[Y];
		geo[Z] = geometry[Z];
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
			info("adding geo %c%c%c%c",
			     alpha_num[geo[A]], alpha_num[geo[X]],
			     alpha_num[geo[Y]], alpha_num[geo[Z]]);
		list_append(geos, geo);
	}
	return 1;
}

static void _internal_removable_set_mps(int level, int *start,
					int *end, int *coords, bool mark)
{
	ba_mp_t *curr_mp;

	if (level > cluster_dims)
		return;

	if (level < cluster_dims) {
		for (coords[level] = start[level];
		     coords[level] < end[level];
		     coords[level]++) {
			/* handle the outter dims here */
			_internal_removable_set_mps(
				level+1, start, end, coords, mark);
		}
		return;
	}

	curr_mp = &ba_main_grid[coords[A]][coords[X]][coords[Y]][coords[Z]];
	if (mark) {
		if (curr_mp->used == BA_MP_USED_FALSE)
			curr_mp->used = BA_MP_USED_TEMP;
	} else {
		if (curr_mp->used == BA_MP_USED_TEMP)
			curr_mp->used = BA_MP_USED_FALSE;
	}
}

/*
 * Fill in the paths and extra midplanes we need for the block.
 * Basically copy the x path sent in with the start_list in each C and
 * D dimension filling in every midplane for the block and then
 * completing the C and D wiring, tying the whole block together.
 *
 * IN/OUT results - total list of midplanes after this function
 *        returns successfully.  Should be
 *        an exact copy of the start_list at first.
 * IN start_list - exact copy of results at first, This should only be
 *        a list of midplanes on the B dim.  We will work off this and
 *        the geometry to fill in this wiring for the B dim in all the
 *        C and D coords.
 * IN geometry - What the block looks like
 * IN conn_type - Mesh or Torus
 *
 * RET: 0 on failure 1 on success
 */
static int _fill_in_coords(List results, int level, ba_mp_t *start_mp,
			   ba_mp_t **check_mp, uint16_t *block_start,
			   uint16_t *block_end, uint16_t *pass_end,
			   int *coords)
{
	int dim;
	int count_over = 0;
	uint16_t used = 0;
	ba_mp_t *curr_mp;

	if (level > cluster_dims)
		return -1;

	if (level < cluster_dims) {
		check_mp[level] = start_mp;
		for (coords[level] = block_start[level];
		     coords[level] <= pass_end[level];
		     coords[level]++) {
			/* handle the outter dims here */
			if (_fill_in_coords(
				    results, level+1, start_mp,
				    check_mp, block_start,
				    block_end, pass_end, coords) == -1)
				return -1;
			check_mp[level] = check_mp[level]->next_mp[level];
		}
		return 1;
	}

	curr_mp = &ba_main_grid[coords[A]][coords[X]][coords[Y]][coords[Z]];

	for (dim=0; dim<cluster_dims; dim++) {
		/* If we get over 2 in any dim that we are
		   greater here we are pass anything we need to
		   passthrough, so break.
		*/
		if (check_mp[dim]->used & BA_MP_USED_PASS_BIT) {
			if (curr_mp->coord[dim] > block_end[dim]) {
				count_over++;
				if (count_over > 1)
					break;
			}
			used = check_mp[dim]->used;
		}
	}
	if (count_over > 1) {
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
			info("skipping non-used %s", curr_mp->coord_str);
		return 1;
	}

	for (dim=0; dim<cluster_dims; dim++) {
		int rc;

		/* If we are passing though skip all except the
		   actual passthrough dim.
		*/
		if ((used & BA_MP_USED_PASS_BIT)
		    && (check_mp[dim]->used != used)) {
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("skipping here %s(%d)",
				     curr_mp->coord_str, dim);
			continue;
		}

		/* if 1 is returned we haven't visited this mp yet,
		   and need to add it to the list
		*/
		if ((rc = _copy_ba_switch(curr_mp, check_mp[dim], dim)) == -1)
			return rc;
		else if (rc == 1)
			list_append(results, curr_mp);
	}
	return 1;
}

static char *_copy_from_main(List main_mps, List ret_list)
{
	ListIterator itr;
	ba_mp_t *ba_mp;
	ba_mp_t *new_mp;
	int dim;
	char *name = NULL;
	hostlist_t hostlist = NULL;

	if (!main_mps || !ret_list)
		return NULL;

	if (!(itr = list_iterator_create(main_mps)))
		fatal("NULL itr returned");
	while ((ba_mp = list_next(itr))) {
		if (!(ba_mp->used & BA_MP_USED_ALTERED)) {
			error("_copy_from_main: it appears we "
			      "have a mp %s added that wasn't altered %d",
			      ba_mp->coord_str, ba_mp->used);
			continue;
		}

		new_mp = ba_copy_mp(ba_mp);
		list_append(ret_list, new_mp);
		/* copy and reset the path */
		memcpy(new_mp->axis_switch, new_mp->alter_switch,
		       sizeof(ba_mp->axis_switch));
		memset(new_mp->alter_switch, 0, sizeof(new_mp->alter_switch));
		if (new_mp->used & BA_MP_USED_PASS_BIT) {
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("_copy_from_main: "
				     "mp %s is used for passthrough",
				     new_mp->coord_str);
			new_mp->used = BA_MP_USED_FALSE;
		} else {
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("_copy_from_main: "
				     "mp %s is used", new_mp->coord_str);
			new_mp->used = BA_MP_USED_TRUE;
			/* Take this away if we decide we don't want
			   this to setup the main list.
			*/
			ba_mp->used = new_mp->used;
			if (hostlist)
				hostlist_push(hostlist, new_mp->coord_str);
			else
				hostlist = hostlist_create(new_mp->coord_str);
		}

		/* reset the main mp */
		ba_mp->used &= (~BA_MP_USED_ALTERED_PASS);
		memset(ba_mp->alter_switch, 0, sizeof(ba_mp->alter_switch));
		/* Take this away if we decide we don't want
		   this to setup the main list.
		*/
		/* info("got usage of %s %d %d", new_mp->coord_str, */
		/*      new_mp->used, ba_mp->used); */
		for (dim=0; dim<cluster_dims; dim++) {
			ba_mp->axis_switch[dim].usage |=
				new_mp->axis_switch[dim].usage;
			/* info("dim %d is %s", dim, */
			/*      ba_switch_usage_str( */
			/* 	     ba_mp->axis_switch[dim].usage)); */
		}
	}
	list_iterator_destroy(itr);

	if (hostlist) {
		name = hostlist_ranged_string_xmalloc(hostlist);
		hostlist_destroy(hostlist);
		color_count++;
	}

	return name;
}

static char *_reset_altered_mps(List main_mps)
{
	ListIterator itr = NULL;
	ba_mp_t *ba_mp;
	char *name = NULL;
	hostlist_t hostlist = NULL;

	xassert(main_mps);

	if (!(itr = list_iterator_create(main_mps)))
		fatal("got NULL list iterator");
	while ((ba_mp = list_next(itr))) {
		if (!(ba_mp->used & BA_MP_USED_ALTERED)) {
			error("_reset_altered_mps: it appears we "
			      "have a mp %s added that wasn't altered",
			      ba_mp->coord_str);
			continue;
		}

		if (ba_mp->used & BA_MP_USED_PASS_BIT) {
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("_reset_altered_mps: "
				     "mp %s is used for passthrough",
				     ba_mp->coord_str);
		} else {
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("_reset_altered_mps: "
				     "mp %s is used", ba_mp->coord_str);
			if (hostlist)
				hostlist_push(hostlist, ba_mp->coord_str);
			else
				hostlist = hostlist_create(ba_mp->coord_str);
		}
		ba_mp->used &= (~BA_MP_USED_ALTERED_PASS);
		memset(ba_mp->alter_switch, 0, sizeof(ba_mp->alter_switch));
	}
	list_iterator_destroy(itr);

	if (hostlist) {
		name = hostlist_ranged_string_xmalloc(hostlist);
		hostlist_destroy(hostlist);
		color_count++;
	}

	return name;
}

static int _copy_ba_switch(ba_mp_t *ba_mp, ba_mp_t *orig_mp, int dim)
{
	int rc = 0;
	if (ba_mp->alter_switch[dim].usage != BG_SWITCH_NONE) {
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
			info("_copy_ba_switch: "
			     "switch already set %s(%d)",
			     ba_mp->coord_str, dim);
		return 0;

	}
	if ((orig_mp->used & BA_MP_USED_PASS_BIT)
	    || (ba_mp->used & BA_MP_USED_PASS_BIT)) {
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
			info("_copy_ba_switch: "
			     "pass bit set %d %d",
			     orig_mp->alter_switch[dim].usage
			     & BG_SWITCH_PASS_FLAG,
			     ba_mp->alter_switch[dim].usage
			     & BG_SWITCH_PASS_FLAG);
		if (!(orig_mp->alter_switch[dim].usage & BG_SWITCH_PASS_FLAG)) {
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("_copy_ba_switch: "
				     "skipping %s(%d)", ba_mp->coord_str, dim);
			return 0;
		}
	} else if (_mp_used(ba_mp, dim)) {
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
			info("_copy_ba_switch: "
			     "%s is already used", ba_mp->coord_str);
		return -1;
	}

	if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
		info("_copy_ba_switch: "
		     "mapping dim %d of %s(%d) to %s(%d) %d",
		     dim, orig_mp->coord_str, orig_mp->used,
		     ba_mp->coord_str, ba_mp->used,
		     orig_mp->used == BA_MP_USED_ALTERED_PASS);
	if (!(ba_mp->used & BA_MP_USED_ALTERED)) {
		if (bg_switch_overlap(ba_mp->axis_switch[dim].usage,
				      orig_mp->alter_switch[dim].usage)) {
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("_copy_ba_switch: "
				     "%s switches %d overlapped %s to %s",
				     ba_mp->coord_str, dim,
				     ba_switch_usage_str(
					     ba_mp->alter_switch[dim].usage),
				     ba_switch_usage_str(
					     orig_mp->alter_switch[dim].usage));
			return -1;
		}
		rc = 1;
	}

	/* Just overlap them here so if they are used in a passthough
	   we get it.
	*/
	ba_mp->used |= orig_mp->used;

	if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
		info("_copy_ba_switch: "
		     "adding from %s %s to %s",
		     ba_switch_usage_str(orig_mp->alter_switch[dim].usage),
		     orig_mp->coord_str,
		     ba_switch_usage_str(ba_mp->alter_switch[dim].usage));
	ba_mp->alter_switch[dim].usage |= orig_mp->alter_switch[dim].usage;

	return rc;
}

static int _check_deny_pass(int dim)
{
	if (!deny_pass || !*deny_pass)
		return 0;

	switch (dim) {
	case A:
		*deny_pass |= PASS_FOUND_A;
		if (*deny_pass & PASS_DENY_A) {
			debug("We don't allow A passthoughs");
			return 1;
		}
		break;
	case X:
		*deny_pass |= PASS_FOUND_X;
		if (*deny_pass & PASS_DENY_X) {
			debug("We don't allow X passthoughs");
			return 1;
		}
		break;
	case Y:
		*deny_pass |= PASS_FOUND_Y;
		if (*deny_pass & PASS_DENY_Y) {
			debug("We don't allow Y passthoughs");
			return 1;
		}
		break;
	case Z:
		*deny_pass |= PASS_FOUND_Z;
		if (*deny_pass & PASS_DENY_Z) {
			debug("We don't allow Z passthoughs");
			return 1;
		}
		break;
	default:
		error("unknown dim %d", dim);
		return 1;
		break;
	}
	return 0;
}

static int _find_path(List mps, ba_mp_t *start_mp, int dim,
		      uint16_t geometry, uint16_t conn_type,
		      uint16_t *block_end, uint16_t *longest)
{
	ba_mp_t *curr_mp = start_mp->next_mp[dim];
	ba_switch_t *axis_switch = NULL;
	ba_switch_t *alter_switch = NULL;
	int count = 1;
	int add = 0;

	if (_mp_used(start_mp, dim))
		return 0;

	//start_mp->used |= BA_MP_USED_START;

	axis_switch = &start_mp->axis_switch[dim];
	alter_switch = &start_mp->alter_switch[dim];
	if (geometry == 1) {
		/* Always check MESH here since we only care about the
		   IN/OUT ports.
		*/
		start_mp->used |= BA_MP_USED_ALTERED;
		if (conn_type == SELECT_TORUS) {
			alter_switch->usage |= BG_SWITCH_WRAPPED;
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
				info("using mp %s(%d) in 1 geo %s added %s",
				     start_mp->coord_str, dim,
				     ba_switch_usage_str(axis_switch->usage),
				     ba_switch_usage_str(alter_switch->usage));
		}
		return 1;
	}
	if (_mp_out_used(start_mp, dim))
		return 0;
	start_mp->used |= BA_MP_USED_ALTERED;
	alter_switch->usage |= BG_SWITCH_OUT;
	alter_switch->usage |= BG_SWITCH_OUT_PASS;

	while (curr_mp != start_mp) {
		xassert(curr_mp);
		//curr_mp->used |= BA_MP_USED_START;
		axis_switch = &curr_mp->axis_switch[dim];
		alter_switch = &curr_mp->alter_switch[dim];
		if (curr_mp->coord[dim] > *longest)
			*longest = curr_mp->coord[dim];
		/* This should never happen since we got here
		   from an unused mp */
		xassert(!(axis_switch->usage & BG_SWITCH_IN_PASS));
		if ((count < geometry) && !_mp_used(curr_mp, dim)) {
			if (curr_mp->coord[dim] > *block_end)
				*block_end = curr_mp->coord[dim];
			count++;
			if (!(curr_mp->used & BA_MP_USED_ALTERED)) {
				add = 1;
				curr_mp->used |= BA_MP_USED_ALTERED;
			}
			alter_switch->usage |= BG_SWITCH_IN_PASS;
			alter_switch->usage |= BG_SWITCH_IN;
			if ((count < geometry) || (conn_type == SELECT_TORUS)) {
				alter_switch->usage |= BG_SWITCH_OUT;
				alter_switch->usage |= BG_SWITCH_OUT_PASS;
				if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
					info("using mp %s(%d) %d(%d) "
					     "%s added %s",
					     curr_mp->coord_str, dim,
					     count, geometry,
					     ba_switch_usage_str(
						     axis_switch->usage),
					     ba_switch_usage_str(
						     alter_switch->usage));
			} else if (conn_type == SELECT_MESH) {
				if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
					info("using mp %s(%d) %d(%d) "
					     "%s added %s",
					     curr_mp->coord_str, dim,
					     count, geometry,
					     ba_switch_usage_str(
						     axis_switch->usage),
					     ba_switch_usage_str(
						     alter_switch->usage));
				if (add)
					list_append(mps, curr_mp);
				return 1;
			}
		} else if (!_mp_out_used(curr_mp, dim)
			   && !_check_deny_pass(dim)) {
			if (!(curr_mp->used & BA_MP_USED_ALTERED)) {
				add = 1;
				curr_mp->used |= BA_MP_USED_ALTERED_PASS;
			}
			alter_switch->usage |= BG_SWITCH_PASS;
			if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP) {
				if (count == geometry) {
					info("using mp %s(%d) to "
					     "finish torus %s added %s",
					     curr_mp->coord_str, dim,
					     ba_switch_usage_str(
						     axis_switch->usage),
					     ba_switch_usage_str(
						     alter_switch->usage));
				} else {
					info("using mp %s(%d) as passthrough "
					     "%s added %s",
					     curr_mp->coord_str, dim,
					     ba_switch_usage_str(
						     axis_switch->usage),
					     ba_switch_usage_str(
						     alter_switch->usage));
				}
			}
		} else {
			/* we can't use this so return with a nice 0 */
			return 0;
		}

		if (add)
			list_append(mps, curr_mp);
		curr_mp = curr_mp->next_mp[dim];
	}

	if (count != geometry)
		return 0;

	if (curr_mp == start_mp) {
		axis_switch = &curr_mp->axis_switch[dim];
		alter_switch = &curr_mp->alter_switch[dim];
		/* This should never happen since we got here
		   from an unused mp */
		xassert(!(axis_switch->usage & BG_SWITCH_IN_PASS));
		alter_switch->usage |= BG_SWITCH_IN_PASS;
		alter_switch->usage |= BG_SWITCH_IN;
	}

	return 1;
}

/** */
static int _setup_next_mps(ba_mp_t ****grid)
{
	int a;
	ba_mp_t *source = NULL, *target = NULL;
	if (cluster_dims == 1) {
		for(a=0;a<DIM_SIZE[A];a++) {
			source = &grid[a][0][0][0];
			if (a<(DIM_SIZE[A]-1))
				target = &grid[a+1][0][0][0];
			else
				target = &grid[0][0][0][0];
			source->next_mp[A] = target;
		}
	} else {
		int x, y, z;
		for (a = 0; a < DIM_SIZE[A]; a++)
			for (x = 0; x < DIM_SIZE[X]; x++)
				for (y = 0; y < DIM_SIZE[Y]; y++)
					for (z = 0; z < DIM_SIZE[Z]; z++) {
						source = &grid[a][x][y][z];

						if (a < (DIM_SIZE[A]-1))
							target = &grid
								[a+1][x][y][z];
						else
							target = &grid
								[0][x][y][z];
						source->next_mp[A] = target;

						if (x < (DIM_SIZE[X]-1))
							target = &grid
								[a][x+1][y][z];
						else
							target = &grid
								[a][0][y][z];
						source->next_mp[X] = target;

						if (y < (DIM_SIZE[Y]-1))
							target = &grid
								[a][x][y+1][z];
						else
							target = &grid
								[a][x][0][z];
						source->next_mp[Y] = target;

						if (z < (DIM_SIZE[Z]-1))
							target = &grid
								[a][x][y][z+1];
						else
							target = &grid
								[a][x][y][0];
						source->next_mp[Z] = target;
					}
	}
	return 1;
}

extern void ba_create_system(int num_cpus, int *real_dims, int *dims)
{
	int a,x,y,z;

	if (ba_main_grid)
		ba_destroy_system();

	memcpy(REAL_DIM_SIZE, real_dims, sizeof(REAL_DIM_SIZE));
	memcpy(DIM_SIZE, dims, sizeof(DIM_SIZE));

	ba_main_grid = (ba_mp_t****)
		xmalloc(sizeof(ba_mp_t***) * DIM_SIZE[A]);
	for (a = 0; a < DIM_SIZE[A]; a++) {
		ba_main_grid[a] = (ba_mp_t***)
			xmalloc(sizeof(ba_mp_t**) * DIM_SIZE[X]);
		for (x = 0; x < DIM_SIZE[X]; x++) {
			ba_main_grid[a][x] = (ba_mp_t**)
				xmalloc(sizeof(ba_mp_t*) * DIM_SIZE[Y]);
			for (y = 0; y < DIM_SIZE[Y]; y++) {
				ba_main_grid[a][x][y] = (ba_mp_t*)
					xmalloc(sizeof(ba_mp_t) * DIM_SIZE[Z]);
				for (z = 0; z < DIM_SIZE[Z]; z++) {
					ba_mp_t *ba_mp = &ba_main_grid
						[a][x][y][z];
					ba_mp->coord[A] = a;
					ba_mp->coord[X] = x;
					ba_mp->coord[Y] = y;
					ba_mp->coord[Z] = z;
					snprintf(ba_mp->coord_str,
						 sizeof(ba_mp->coord_str),
						 "%c%c%c%c",
						 alpha_num[ba_mp->coord[A]],
						 alpha_num[ba_mp->coord[X]],
						 alpha_num[ba_mp->coord[Y]],
						 alpha_num[ba_mp->coord[Z]]);
					ba_setup_mp(ba_mp, true);
				}
			}
		}
	}

	if (cluster_flags & CLUSTER_FLAG_BGQ)
		_setup_next_mps(ba_main_grid);
}

/** */
extern void ba_destroy_system(void)
{
	int a, x, y;

	if (ba_main_grid) {
		for (a=0; a<DIM_SIZE[A]; a++) {
			for (x = 0; x < DIM_SIZE[X]; x++) {
				for (y = 0; y < DIM_SIZE[Y]; y++)
					xfree(ba_main_grid[a][x][y]);
				xfree(ba_main_grid[a][x]);
			}
			xfree(ba_main_grid[a]);
		}
		xfree(ba_main_grid);
		ba_main_grid = NULL;
	}

	memset(DIM_SIZE, 0, sizeof(DIM_SIZE));
}

/**
 * algorithm for finding match
 */
static int _find_match(select_ba_request_t *ba_request, List results)
{
	int i=0;
	uint16_t start[cluster_dims];
	uint16_t end[cluster_dims];
	char *name=NULL;
	int startx;
	uint16_t *geo_ptr;

	if (!(cluster_flags & CLUSTER_FLAG_BG))
		return 0;

	memset(start, 0, sizeof(start));
	startx = (start[X]-1);

	if (startx == -1)
		startx = DIM_SIZE[X]-1;
	if (ba_request->start_req) {
		for (i = 0; i < cluster_dims; i++) {
			if (ba_request->start[i] >= DIM_SIZE[i])
				return 0;
			start[i] = ba_request->start[i];
		}
	}

	/* set up the geo here */
	if (!(geo_ptr = (uint16_t *)list_peek(ba_request->elongate_geos)))
		return 0;
	ba_request->rotate_count=0;
	ba_request->elongate_count=1;

	for (i = 0; i < cluster_dims; i++)
		ba_request->geometry[i] = geo_ptr[i];

	for (i = 0; i < cluster_dims; i++) {
		end[i] = start[i] + ba_request->geometry[i];
		if (end[i] > DIM_SIZE[i])
			if (!_check_for_options(ba_request))
				return 0;
	}

start_again:
	i = 0;
	if (i == startx)
		i = startx-1;
	while (i != startx) {
		i++;
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
			info("finding %c%c%c%c try %d",
			     alpha_num[ba_request->geometry[A]],
			     alpha_num[ba_request->geometry[X]],
			     alpha_num[ba_request->geometry[Y]],
			     alpha_num[ba_request->geometry[Z]],
			     i);
	new_mp:
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO)
			info("starting at %c%c%c%c",
			     alpha_num[start[A]],
			     alpha_num[start[X]],
			     alpha_num[start[Y]],
			     alpha_num[start[Z]]);

		if ((name = set_bg_block(results, start,
					 ba_request->geometry,
					 ba_request->conn_type))) {
			ba_request->save_name = name;
			name = NULL;
			return 1;
		}

		if (results) {
			bool is_small = 0;
			if (ba_request->conn_type[0] == SELECT_SMALL)
				is_small = 1;
			remove_block(results, color_count, is_small);
			list_flush(results);
		}

		if (ba_request->start_req)
			goto requested_end;
		//exit(0);
		debug2("trying something else");

		if ((DIM_SIZE[Z]-start[Z]-1) >= ba_request->geometry[Z])
			start[Z]++;
		else {
			start[Z] = 0;
			if ((DIM_SIZE[Y]-start[Y]-1) >= ba_request->geometry[Y])
				start[Y]++;
			else {
				start[Y] = 0;
				if ((DIM_SIZE[X]-start[X]-1)
				    >= ba_request->geometry[X])
					start[X]++;
				else {
					start[X] = 0;
					if ((DIM_SIZE[A]-start[A]-1)
					    >= ba_request->geometry[A])
						start[A]++;
					else {
						if (ba_request->size == 1)
							goto requested_end;
						if (!_check_for_options(
							    ba_request))
							return 0;
						else {
							start[A]=0;
							start[X]=0;
							start[Y]=0;
							start[Z]=0;
							goto start_again;
						}
					}
				}
			}
		}
		goto new_mp;
	}

requested_end:
	debug2("1 can't allocate");

	return 0;
}

/*
 * Used to check if midplane is usable in the block we are creating
 *
 * IN: ba_mp - mp to check if is used
 * IN: dim - dimension we are checking.
  */
static bool _mp_used(ba_mp_t* ba_mp, int dim)
{
	xassert(ba_mp);

	/* if we've used this mp in another block already */
	if (mp_strip_unaltered(ba_mp->used)
	    || ba_mp->axis_switch[dim].usage & BG_SWITCH_WRAPPED) {
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
			info("mp %s used in the %d dim (%d, %s)",
			     ba_mp->coord_str, dim,
			     mp_strip_unaltered(ba_mp->used),
			     ba_switch_usage_str(
				     ba_mp->axis_switch[dim].usage));
		return true;
	}
	return false;
}

/*
 * Used to check if we can leave a midplane
 *
 * IN: ba_mp - mp to check if is used
 * IN: dim - dimension we are checking.
 */
static bool _mp_out_used(ba_mp_t* ba_mp, int dim)
{
	xassert(ba_mp);

	/* If the mp is already used just check the PASS_USED. */
	if (ba_mp->axis_switch[dim].usage & BG_SWITCH_PASS_USED) {
		if (ba_debug_flags & DEBUG_FLAG_BG_ALGO_DEEP)
			info("passthroughs used in the %d dim on mp %s (%s)",
			     dim, ba_mp->coord_str, ba_switch_usage_str(
				     ba_mp->axis_switch[dim].usage));
		return true;
	}

	return false;
}

static void _destroy_geo(void *object)
{
	uint16_t *geo_ptr = (uint16_t *)object;
	xfree(geo_ptr);
}

static int _coord(char coord)
{
	if ((coord >= '0') && (coord <= '9'))
		return (coord - '0');
	if ((coord >= 'A') && (coord <= 'Z'))
		return (coord - 'A');
	return -1;
}
