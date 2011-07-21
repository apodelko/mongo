/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_col_search --
 *	Search a column-store tree for a specific record-based key.
 */
int
__wt_col_search(WT_SESSION_IMPL *session, uint64_t recno, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_COL_REF *cref;
	WT_INSERT *ins;
	WT_PAGE *page;
	WT_UPDATE *upd;
	uint64_t record_cnt, start_recno;
	uint32_t base, i, indx, limit, match, slot, write_gen;
	int ret;
	void *cipdata;

	unpack = &_unpack;
	cref = NULL;
	start_recno = 0;
	cipdata = NULL;

	session->srch_page = NULL;			/* Return values. */
	session->srch_write_gen = 0;
	session->srch_ip = NULL;
	session->srch_vupdate = NULL;
	session->srch_ins = NULL;
	session->srch_upd = NULL;
	session->srch_slot = UINT32_MAX;

	btree = session->btree;

	/* Search the tree. */
	for (page = btree->root_page.page; page->type == WT_PAGE_COL_INT;) {
		/* Binary search of internal pages. */
		for (base = 0,
		    limit = page->entries; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			cref = page->u.col_int.t + indx;

			start_recno = cref->recno;
			if (recno == start_recno)
				break;
			if (recno < start_recno)
				continue;
			base = indx + 1;
			--limit;
		}
		WT_ASSERT(session, cref != NULL);

		/*
		 * Reference the slot used for next step down the tree.
		 *
		 * Base is the smallest index greater than recno and may be the
		 * (last + 1) index.  The slot for descent is the one before
		 * base.
		 */
		if (recno != start_recno) {
			/*
			 * We don't have to correct for base == 0 because the
			 * only way for base to be 0 is if recno is the page's
			 * starting recno.
			 */
			WT_ASSERT(session, base > 0);
			cref = page->u.col_int.t + (base - 1);
		}

		/* Swap the parent page for the child page. */
		WT_ERR(__wt_page_in(session, page, &cref->ref, 0));
		if (page != btree->root_page.page)
			__wt_hazard_clear(session, page);
		page = WT_COL_REF_PAGE(cref);
	}

	/*
	 * Copy the page's write generation value before reading anything on
	 * the page.
	 */
	write_gen = page->write_gen;

	/*
	 * Search the leaf page.  We do not check in the search path for a
	 * record greater than the maximum record in the tree; in that case,
	 * we arrive here with a record that's impossibly large for the page.
	 */
	cip = NULL;
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (recno >= page->u.col_leaf.recno + page->entries) {
			if (LF_ISSET(WT_WRITE))
				goto append;
			goto notfound;
		}
		cip = page->u.col_leaf.d + (recno - page->u.col_leaf.recno);
		cipdata = WT_COL_PTR(page, cip);
		break;
	case WT_PAGE_COL_VAR:
		/* Walk the page, counting records. */
		record_cnt = page->u.col_leaf.recno - 1;
		WT_COL_FOREACH(page, cip, i) {
			if ((cipdata = WT_COL_PTR(page, cip)) == NULL)
				++record_cnt;
			else {
				__wt_cell_unpack(cipdata, unpack);
				record_cnt += unpack->rle;
			}
			if (record_cnt >= recno)
				break;
		}
		if (record_cnt < recno) {
			if (LF_ISSET(WT_WRITE))
				goto append;
			goto notfound;
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/*
	 * We have the right WT_COL slot: if it's a write, set up the return
	 * information in session->{srch_upd,slot}.  If it's a read, set up
	 * the return information in session->srch_vupdate.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		slot = WT_COL_SLOT(page, cip);
		if (page->u.col_leaf.upd == NULL)
			session->srch_slot = slot;
		else {
			session->srch_upd = &page->u.col_leaf.upd[slot];
			session->srch_vupdate = page->u.col_leaf.upd[slot];
		}

		/*
		 * If writing data, we're done, we don't care if the item was
		 * deleted or not.
		 */
		if (LF_ISSET(WT_WRITE))
			break;

		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			if (WT_UPDATE_DELETED_ISSET(upd))
				goto notfound;
			session->srch_vupdate = upd;
		} else if (page->type == WT_PAGE_COL_FIX) {
			if (WT_FIX_DELETE_ISSET(cipdata))
				goto notfound;
		} else {
			__wt_cell_unpack(cipdata, unpack);
			if (unpack->type == WT_CELL_DEL)
				goto notfound;
		}
		break;
	case WT_PAGE_COL_VAR:
		/*
		 * Search the WT_COL's insert list for the record's WT_INSERT
		 * slot.  The insert list is a sorted, forward-linked list --
		 * on average, we have to search half of it.
		 *
		 * Do an initial setup of the return information (we'll correct
		 * it as needed depending on what we find).
		 */
		session->srch_slot = WT_COL_SLOT(page, cip);
		if (page->u.col_leaf.ins != NULL)
			session->srch_ins =
			    &page->u.col_leaf.ins[session->srch_slot];

		for (match = 0, ins =
		    WT_COL_INSERT(page, cip); ins != NULL; ins = ins->next) {
			if (WT_INSERT_RECNO(ins) == recno) {
				match = 1;
				session->srch_ins = NULL;
				session->srch_vupdate = ins->upd;
				session->srch_upd = &ins->upd;
				break;
			}
			if (WT_INSERT_RECNO(ins) > recno)
				break;
			session->srch_ins = &ins->next;
		}

		/*
		 * If we're not updating an existing data item, check to see if
		 * the item has been deleted.   If we found a match, use the
		 * WT_INSERT's WT_UPDATE value.   If we didn't find a match, use
		 * use the original data.
		 */
		if (LF_ISSET(WT_WRITE))
			break;

		if (match) {
			if (WT_UPDATE_DELETED_ISSET(ins->upd))
				goto notfound;
		} else
			if (cipdata != NULL && unpack->type == WT_CELL_DEL)
				goto notfound;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	session->srch_match = 1;
	if (0) {
append:		session->srch_match = 0;
	}
	session->srch_page = page;
	session->srch_write_gen = write_gen;
	session->srch_ip = cip;
	return (0);

notfound:
	ret = WT_NOTFOUND;

err:	__wt_page_out(session, page);
	return (ret);
}
