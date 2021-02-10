/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */


#include <sys/dmu.h>
#include <sys/dmu_impl.h>
#include <sys/dbuf.h>
#include <sys/dnode.h>
#include <sys/zfs_context.h>
#include <sys/dsl_dataset.h>
#include <sys/dmu_objset.h>

/*
 * Normally the db_blkptr points to the most recent on-disk content for the
 * dbuf (and anything newer will be cached in the dbuf). However, a recent
 * Direct IO write could leave newer content on disk and the dbuf uncached.
 * In this case we must return the (as yet unsynced) pointer to the latest
 * on-disk content.
 *
 * The db_mtx must be held before calling this.
 */
static blkptr_t *
dmu_get_bp_from_dbuf(dmu_buf_impl_t *db)
{
	ASSERT(MUTEX_HELD(&db->db_mtx));

	if (db->db_level != 0) {
		return (db->db_blkptr);
	}

	blkptr_t *bp = db->db_blkptr;

	dbuf_dirty_record_t *dr_head = list_head(&db->db_dirty_records);
	if (dr_head && dr_head->dt.dl.dr_override_state == DR_OVERRIDDEN) {
		/* we have a Direct IO write, use it's bp */
		ASSERT(db->db_state != DB_NOFILL);
		bp = &dr_head->dt.dl.dr_overridden_by;
	}

	return (bp);
}

static void
make_abd_for_dbuf(dmu_buf_impl_t *db, abd_t *data,
    uint64_t offset, uint64_t size, abd_t **buf, abd_t **mbuf)
{
	size_t buf_size = db->db.db_size;
	abd_t *pre_buf = NULL, *post_buf = NULL;
	size_t buf_off = 0;
	abd_t *in_buf = *buf;

	ASSERT(MUTEX_HELD(&db->db_mtx));

	IMPLY(db->db_state == DB_CACHED, db->db.db_data != NULL);
	if (offset > db->db.db_offset) {
		size_t pre_size = offset - db->db.db_offset;
		if (db->db_state == DB_CACHED)
			pre_buf = abd_get_from_buf(db->db.db_data, pre_size);
		else if (in_buf)
			pre_buf = abd_get_offset_size(in_buf, 0, pre_size);
		else
			pre_buf = abd_alloc_for_io(pre_size, B_TRUE);
		buf_size -= pre_size;
		buf_off = 0;
	} else {
		buf_off = db->db.db_offset - offset;
		size -= buf_off;
	}

	if (size < buf_size) {
		size_t post_size = buf_size - size;
		if (db->db_state == DB_CACHED)
			post_buf = abd_get_from_buf(
			    (char *)db->db.db_data + db->db.db_size - post_size,
			    post_size);
		else if (in_buf)
			post_buf = abd_get_offset_size(in_buf,
			    db->db.db_size - post_size, post_size);
		else
			post_buf = abd_alloc_for_io(post_size, B_TRUE);
		buf_size -= post_size;
	}

	ASSERT3U(buf_size, >, 0);
	*buf = abd_get_offset_size(data, buf_off, buf_size);

	if (pre_buf || post_buf) {
		*mbuf = abd_alloc_gang();
		if (pre_buf)
			abd_gang_add(*mbuf, pre_buf, B_TRUE);
		abd_gang_add(*mbuf, *buf, B_TRUE);
		if (post_buf)
			abd_gang_add(*mbuf, post_buf, B_TRUE);
	} else {
		*mbuf = *buf;
	}
}

static void
dmu_read_abd_done(zio_t *zio)
{
	abd_free(zio->io_abd);
}

static void
dmu_write_direct_ready(zio_t *zio)
{
	dmu_sync_ready(zio, NULL, zio->io_private);
}

static void
dmu_write_direct_done(zio_t *zio)
{
	dmu_sync_arg_t *dsa = zio->io_private;
	dbuf_dirty_record_t *dr = dsa->dsa_dr;
	dmu_buf_impl_t *db = dr->dr_dbuf;

	abd_free(zio->io_abd);

	mutex_enter(&db->db_mtx);
	if (db->db_buf) {
		arc_buf_t *buf = db->db_buf;

		/*
		 * The current contents of the dbuf are now stale.
		 */
		ASSERT(db->db_buf == dr->dt.dl.dr_data);

		db->db_buf = NULL;
		db->db.db_data = NULL;
		dr->dt.dl.dr_data = NULL;

		/*
		 * We must Remove any dirty data that might share the same
		 * ARC buf as we are going to destroy the ARC buf for this
		 * dbuf. It is possible that a dirty record will have already
		 * destroyed the shared ARC buf in dmu_buf_undirty(), which
		 * signaled by db_dirty_arcbuf_destroyed.
		 *
		 * Since we only allow block aligned Direct IO writes we
		 * grabbed the first dirty record of the dbuf in
		 * dmu_write_direct(). Since the rangelocks will prevent
		 * another writer from adding to the db_dirty_records, we
		 * can just grab the next dirty record from the list to
		 * start checking for shared ARC bufs.
		 */
		dr = list_next(&db->db_dirty_records, dr);
		while (dr != NULL) {
			dr = dmu_buf_undirty(db, dr, buf);
			if (db->db_dirty_arcbuf_destroyed == B_TRUE)
				break;
		}

		if (db->db_dirty_arcbuf_destroyed == B_FALSE)
			arc_buf_destroy(buf, db);
	}
	ASSERT(db->db.db_data == NULL);
	db->db_state = DB_UNCACHED;
	mutex_exit(&db->db_mtx);

	dmu_sync_done(zio, NULL, zio->io_private);
	kmem_free(zio->io_bp, sizeof (blkptr_t));
}

int
dmu_write_direct(dmu_buf_impl_t *db, abd_t *data, dmu_tx_t *tx)
{
	objset_t *os = db->db_objset;
	dsl_dataset_t *ds = os->os_dsl_dataset;
	dbuf_dirty_record_t *dr_head, *dr_next;
	dmu_sync_arg_t *dsa;
	zbookmark_phys_t zb;
	zio_prop_t zp;
	dnode_t *dn;
	uint64_t txg = dmu_tx_get_txg(tx);
	blkptr_t *bp;
	zio_t *zio;
	int err = 0;

	ASSERT(tx != NULL);

	SET_BOOKMARK(&zb, ds->ds_object,
	    db->db.db_object, db->db_level, db->db_blkid);

	/*
	 * No support for this
	 */
	if (txg > spa_freeze_txg(os->os_spa))
		return (SET_ERROR(ENOTSUP));

	DB_DNODE_ENTER(db);
	dn = DB_DNODE(db);
	dmu_write_policy(os, dn, db->db_level, WP_DMU_SYNC | WP_DIRECT_WR, &zp);

	DB_DNODE_EXIT(db);

	/*
	 * Dirty this dbuf with DB_NOFILL since we will not have any data
	 * associated with the dbuf.
	 */
	dmu_buf_will_not_fill(&db->db, tx);

	/* XXX - probably don't need this, since we are in an open tx */
	mutex_enter(&db->db_mtx);

	ASSERT(txg > spa_last_synced_txg(os->os_spa));
	ASSERT(txg > spa_syncing_txg(os->os_spa));

	dr_head = list_head(&db->db_dirty_records);
	dr_next = list_next(&db->db_dirty_records, dr_head);
	VERIFY(dr_head->dr_txg == txg);

	bp = kmem_alloc(sizeof (blkptr_t), KM_SLEEP);
	if (db->db_blkptr != NULL) {
		/*
		 * fill in bp with current blkptr so that
		 * the nopwrite code can check if we're writing the same
		 * data that's already on disk.
		 */
		*bp = *db->db_blkptr;
	} else {
		bzero(bp, sizeof (blkptr_t));
	}

	/*
	 * Disable nopwrite if the current BP could change before
	 * this TXG syncs.
	 */
	if (dr_next != NULL)
		zp.zp_nopwrite = B_FALSE;

	ASSERT(dr_head->dt.dl.dr_override_state == DR_NOT_OVERRIDDEN);
	dr_head->dt.dl.dr_override_state = DR_IN_DMU_SYNC;
	mutex_exit(&db->db_mtx);

	/*
	 * We will not be writing this block in syncing context, so
	 * update the dirty space accounting.
	 * XXX - this should be handled as part of will_not_fill()
	 */
	dsl_pool_undirty_space(dmu_objset_pool(os), dr_head->dr_accounted, txg);

	dsa = kmem_alloc(sizeof (dmu_sync_arg_t), KM_SLEEP);
	dsa->dsa_dr = dr_head;
	dsa->dsa_done = NULL;
	dsa->dsa_zgd = NULL;
	dsa->dsa_tx = NULL;

	zio = zio_write(NULL, os->os_spa, txg, bp, data,
	    db->db.db_size, db->db.db_size, &zp,
	    dmu_write_direct_ready, NULL, NULL, dmu_write_direct_done, dsa,
	    ZIO_PRIORITY_SYNC_WRITE, ZIO_FLAG_CANFAIL, &zb);

	err = zio_wait(zio);

	return (err);
}

int
dmu_write_abd(dnode_t *dn, uint64_t offset, uint64_t size,
    abd_t *data, uint32_t flags, dmu_tx_t *tx)
{
	dmu_buf_t **dbp;
	int numbufs, err;

	ASSERT(flags & DMU_DIRECTIO);

	err = dmu_buf_hold_array_by_dnode(dn, offset,
	    size, B_FALSE, FTAG, &numbufs, &dbp, 0);
	if (err)
		return (err);

	for (int i = 0; err == 0 && i < numbufs; i++) {
		dmu_buf_impl_t *db = (dmu_buf_impl_t *)dbp[i];

		abd_t *abd = abd_get_offset_size(data,
		    db->db.db_offset - offset, dn->dn_datablksz);

		err = dmu_write_direct(db, abd, tx);
	}

	dmu_buf_rele_array(dbp, numbufs, FTAG);

	return (err);
}

int
dmu_read_abd(dnode_t *dn, uint64_t offset, uint64_t size,
    abd_t *data, uint32_t flags)
{
	spa_t *spa = dn->dn_objset->os_spa;
	dmu_buf_t **dbp;
	int numbufs, err;
	zio_t *rio;

	ASSERT(flags & DMU_DIRECTIO);

	err = dmu_buf_hold_array_by_dnode(dn, offset,
	    size, B_FALSE, FTAG, &numbufs, &dbp, 0);
	if (err)
		return (err);

	rio = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);

	for (int i = 0; i < numbufs; i++) {
		dmu_buf_impl_t *db = (dmu_buf_impl_t *)dbp[i];
		abd_t *buf = NULL, *mbuf;
		zio_t *zio;

		mutex_enter(&db->db_mtx);
		blkptr_t *bp = dmu_get_bp_from_dbuf(db);

		/* no need to read if hole or data is cached */
		if (bp == NULL || BP_IS_HOLE(bp) || db->db_state == DB_CACHED) {
			size_t aoff = offset < db->db.db_offset ?
			    db->db.db_offset - offset : 0;
			size_t boff = offset > db->db.db_offset ?
			    offset - db->db.db_offset : 0;
			size_t len = MIN(size - aoff, db->db.db_size - boff);
			if (db->db_state == DB_CACHED)
				abd_copy_from_buf_off(data,
				    (char *)db->db.db_data + boff, aoff, len);
			else
				abd_zero_off(data, aoff, len);
			mutex_exit(&db->db_mtx);
			continue;
		}

		make_abd_for_dbuf(db, data, offset, size, &buf, &mbuf);
		mutex_exit(&db->db_mtx);

		zio = zio_read(rio, spa, bp, mbuf, db->db.db_size,
		    dmu_read_abd_done, NULL,
		    ZIO_PRIORITY_SYNC_READ, 0, NULL);

		if (i+1 == numbufs)
			err = zio_wait(zio);
		else
			zio_nowait(zio);
	}

	if (err)
		(void) zio_wait(rio);
	else
		err = zio_wait(rio);

	dmu_buf_rele_array(dbp, numbufs, FTAG);

	return (err);
}

#ifdef _KERNEL
int
dmu_rw_uio_direct(dnode_t *dn, zfs_uio_t *uio, uint64_t size,
    dmu_tx_t *tx, boolean_t read)
{
	offset_t offset = zfs_uio_offset(uio);
	offset_t page_index = (offset - zfs_uio_soffset(uio)) / PAGE_SIZE;
	unsigned long page_offset = offset % PAGE_SIZE;
	abd_t *data;
	int err;

	ASSERT(uio->uio_extflg & UIO_DIRECT);
	ASSERT3U(page_index, <, uio->uio_dio.num_pages);

	data = abd_alloc_from_pages(
	    &uio->uio_dio.pages[page_index], page_offset, size);

	if (read)
		err = dmu_read_abd(dn, offset, size, data, DMU_DIRECTIO);
	else
		err = dmu_write_abd(dn, offset, size, data, DMU_DIRECTIO, tx);

	abd_free(data);

	if (err == 0)
		zfs_uioskip(uio, size);
	return (err);
}
#endif /* _KERNEL */
