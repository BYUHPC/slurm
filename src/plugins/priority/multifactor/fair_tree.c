/*****************************************************************************\
 *  fair_tree.c - Fair Tree fairshare algorithm for Slurm
 *****************************************************************************
 *
 *  Copyright (C) 2014 Brigham Young University
 *  Authors: Ryan Cox <ryan_cox@byu.edu>, Levi Morrison <levi_morrison@byu.edu>
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

#include <math.h>

#include "fair_tree.h"

static void _ft_decay_apply_new_usage(struct job_record *job, time_t *start);
static void _apply_priority_fs(void);

extern void fair_tree_init(void) {
}


/* Fair Tree code called from the decay thread loop */
extern void fair_tree_decay(List jobs, time_t start)
{
	slurmctld_lock_t job_write_lock =
		{ NO_LOCK, WRITE_LOCK, READ_LOCK, READ_LOCK };
	assoc_mgr_lock_t locks =
		{ WRITE_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK };

	/* apply decayed usage */
	lock_slurmctld(job_write_lock);
	list_for_each(jobs, (ListForF) _ft_decay_apply_new_usage, &start);
	unlock_slurmctld(job_write_lock);

	/* calculate fs factor for associations */
	assoc_mgr_lock(&locks);
	_apply_priority_fs();
	assoc_mgr_unlock(&locks);

	/* assign job priorities */
	lock_slurmctld(job_write_lock);
	list_for_each(jobs, (ListForF) decay_apply_weighted_factors, &start);
	unlock_slurmctld(job_write_lock);
}


/* In Fair Tree, usage_efctv is the normalized usage within the account */
static void _ft_set_assoc_usage_efctv(
		slurmdb_association_rec_t *assoc)
{
	slurmdb_association_rec_t *parent = assoc->usage->fs_assoc_ptr;

	if (!parent || !parent->usage->usage_raw) {
		assoc->usage->usage_efctv = 0L;
		return;
	}

	assoc->usage->usage_efctv =
		assoc->usage->usage_raw / (long double)parent->usage->usage_raw;
}


/* Apply usage with decay factor. Call standard functions */
static void _ft_decay_apply_new_usage(struct job_record *job, time_t *start)
{
	if (!decay_apply_new_usage(job, start))
		return;

	/* Priority 0 is reserved for held jobs. Also skip priority
	 * calculation for non-pending jobs. */
	if ((job->priority == 0) || !IS_JOB_PENDING(job))
		return;

	set_priority_factors(*start, job);
	last_job_update = time(NULL);
}


static void _ft_debug(slurmdb_association_rec_t *assoc,
		uint16_t assoc_level, bool tied)
{
	int spaces;
	char *name;
	int tie_char_count = tied ? 1 : 0;

	spaces = (assoc_level + 1) * 4;
	name = assoc->user ? assoc->user : assoc->acct;


	info("%*s%.*s%s (%s):  %.20Lf",
	       spaces,
	       "",
	       tie_char_count,
	       "=",
	       name,
	       assoc->acct,
	       assoc->usage->level_fs);
}


/* Sort so that higher level_fs values are first in the list */
static int _sort_level_fs(slurmdb_association_rec_t **x,
					 slurmdb_association_rec_t **y)
{
	/* We sort based on the following critereon:
	 *  1. level_fs value
	 *  2. Prioritize users over accounts (required for tie breakers when
	 *     comparing users and accounts)
	 */
	slurmdb_association_rec_t *a = *x;
	slurmdb_association_rec_t *b = *y;

	/* 1. level_fs value */
	if (a->usage->level_fs != b->usage->level_fs)
		return a->usage->level_fs < b->usage->level_fs ? 1 : -1;

	/* 2. Prioritize users over accounts */

	/* a and b are both users or both accounts */
	if (!a->user == !b->user)
		return 0;

	/* -1 if a is user, 1 if b is user */
	return a->user ? -1 : 1;
}


/* Calculate F=2**(-Ueff/S) for an association. */
static void _calc_assoc_fs(slurmdb_association_rec_t *assoc)
{
	long double shares_adj = 0L;

	_ft_set_assoc_usage_efctv(assoc);

	/* Fair Tree doesn't use usage_norm but we will set it anyway */
	set_assoc_usage_norm(assoc);

	if (assoc->shares_raw == SLURMDB_FS_USE_PARENT) {
		if (assoc->user)
			assoc->usage->level_fs = 1L;
		else
			assoc->usage->level_fs = 0L;
		return;
	}

	/* If shares_norm is zero, the association might be new or you really
	 * don't like them. Either way, give them no fairshare. */
	if (!assoc->usage->shares_norm) {
		assoc->usage->level_fs = 0L;
		return;
	}

	/* This function normalizes shares to be between 0.1 and 1.0;
	 * this range fares much better than 0.0 to 1.0 when used in
	 * the denominator of the fairshare calculation:
	 *   2**(-UsageEffective / Shares)
	 *
	 * Compare these two:
	 * http://www.wolframalpha.com/input/?i=2%5E-%28u%2Fs%29%2C+u+from+0+to+1%2C+s+from+.1+to+1
	 * http://www.wolframalpha.com/input/?i=2%5E-%28u%2Fs%29%2C+u+from+0+to+1%2C+s+from+0+to+1
	 */
	shares_adj = lerp(0.1L, 1L, assoc->usage->shares_norm);
	assoc->usage->level_fs =
#if defined(__FreeBSD__)
		pow(2L, -(assoc->usage->usage_efctv / shares_adj));
#else
		powl(2L, -(assoc->usage->usage_efctv / shares_adj));
#endif
}


/* This function merges the children of any tied sibling accounts into a single
 * list that is later sorted by level_fs and shares_norm.
 *
 * itr is the list iterator for this association's siblings, not merged_list.
 */
static void _merge_tied_accounts(List merged_list,
		slurmdb_association_rec_t *assoc, ListIterator itr,
		uint16_t assoc_level)
{
	slurmdb_association_rec_t *next_assoc;

	if (assoc->usage->children_list)
		list_append_list(merged_list, assoc->usage->children_list);

	/* If next_assoc is an account and its level_fs and shares_norm are
	 * equal to assoc's, append its children_list to merged_list */
	while ((next_assoc = list_iterator_peek(itr))) {
		if (next_assoc->user ||
		(assoc->usage->level_fs != next_assoc->usage->level_fs))
			break;
		if (priority_debug)
			_ft_debug(next_assoc, assoc_level, 1);

		if (next_assoc->usage->children_list)
			list_append_list(merged_list,
				next_assoc->usage->children_list);

		/* Skip next_assoc since its children are now merged */
		list_next(itr);
	}

}


/* Calculate fairshare for each child then sort children by fairshare value
 * (level_fs). Once they are sorted, operate on each child in sorted order.
 * This portion of the tree is now sorted and users are given a fairshare value
 * based on the order they are operated on. The basic equation is
 * (rank / g_user_assoc_count), though ties are allowed. The rank is decremented
 * for each user that is encountered.
 */
static void _calc_tree_fs(List children_list, uint16_t assoc_level,
		uint32_t *rank, uint32_t *i, bool account_tied)
{
	List children_copy = NULL;
	ListIterator itr = NULL;
	slurmdb_association_rec_t *assoc = NULL;
	long double prev_level_fs = (long double) NO_VAL;
	bool tied = false;

	if (!children_list || !list_count(children_list))
		return;

	/* Calculate level_fs for each child */
	list_for_each(children_list, (ListForF) _calc_assoc_fs,	NULL);

	/* Sort children by level_fs. children_list was passed in as a copy so
	 * sorting it is safe */
	list_sort(children_list, (ListCmpF) _sort_level_fs);

	/* Iterate through children in sorted order. If it's a user, calculate
	 * fs_factor, otherwise recurse. */
	itr = list_iterator_create(children_list);
	while ((assoc = list_next(itr))) {
		if (account_tied) {
			tied = true;
			account_tied = false;
		} else {
			tied = prev_level_fs == assoc->usage->level_fs;
		}

		if (priority_debug)
			_ft_debug(assoc, assoc_level, tied);
		if (assoc->user) {
			if (!tied)
				*rank = *i;

			/* Set the final fairshare factor for this user */
			assoc->usage->fs_factor =
				*rank / (double) g_user_assoc_count;
			(*i)--;
		} else {
			children_copy = list_create(NULL);

			/* Merging does not affect child level_fs calculations
			 * since the necessary information is stored on each
			 * assoc's usage struct */
			_merge_tied_accounts(children_copy, assoc, itr,
					assoc_level);

			_calc_tree_fs(children_copy, assoc_level + 1, rank, i,
					tied);

			list_destroy(children_copy);
		}
		prev_level_fs = assoc->usage->level_fs;
	}

	list_iterator_destroy(itr);
}


/* Start fairshare calculations at root. Call assoc_mgr_lock before this. */
static void _apply_priority_fs(void)
{
	List children_copy = NULL;
	uint32_t rank = g_user_assoc_count;
	uint32_t i = rank;

	if (priority_debug)
		info("Fair Tree fairshare algorithm, starting at root:");

	assoc_mgr_root_assoc->usage->level_fs = 1L;

	/* _calc_tree_fs requires a copy of children_list */
	children_copy = list_create(NULL);
	list_append_list(children_copy,
			assoc_mgr_root_assoc->usage->children_list);

	_calc_tree_fs(children_copy, 0, &rank, &i, false);
	list_destroy(children_copy);
}
