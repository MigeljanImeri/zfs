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
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T	*/
/*	  All Rights Reserved	*/

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */
/*
 * Copyright (c) 2015 by Chunwei Chen. All rights reserved.
 */

#ifdef _KERNEL

#include <sys/errno.h>
#include <sys/vmem.h>
#include <sys/strings.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/uio_impl.h>
#include <sys/zfs_debug.h>
#include <linux/kmap_compat.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>

#include <sys/zfs_znode.h>

/*
 * Move "n" bytes at byte address "p"; "rw" indicates the direction
 * of the move, and the I/O parameters are provided in "uio", which is
 * update to reflect the data which was moved.  Returns 0 on success or
 * a non-zero errno on failure.
 */
static int
zfs_uiomove_iov(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	const struct iovec *iov = uio->uio_iov;
	size_t skip = uio->uio_skip;
	ulong_t cnt;

	while (n && uio->uio_resid) {
		cnt = MIN(iov->iov_len - skip, n);
		switch (uio->uio_segflg) {
		case UIO_USERSPACE:
			/*
			 * p = kernel data pointer
			 * iov->iov_base = user data pointer
			 */
			if (rw == UIO_READ) {
				if (copy_to_user(iov->iov_base+skip, p, cnt))
					return (EFAULT);
			} else {
				unsigned long b_left = 0;
				if (uio->uio_fault_disable) {
					if (!zfs_access_ok(VERIFY_READ,
					    (iov->iov_base + skip), cnt)) {
						return (EFAULT);
					}
					pagefault_disable();
					b_left =
					    __copy_from_user_inatomic(p,
					    (iov->iov_base + skip), cnt);
					pagefault_enable();
				} else {
					b_left =
					    copy_from_user(p,
					    (iov->iov_base + skip), cnt);
				}
				if (b_left > 0) {
					unsigned long c_bytes =
					    cnt - b_left;
					uio->uio_skip += c_bytes;
					ASSERT3U(uio->uio_skip, <,
					    iov->iov_len);
					uio->uio_resid -= c_bytes;
					uio->uio_loffset += c_bytes;
					return (EFAULT);
				}
			}
			break;
		case UIO_SYSSPACE:
			if (rw == UIO_READ)
				bcopy(p, iov->iov_base + skip, cnt);
			else
				bcopy(iov->iov_base + skip, p, cnt);
			break;
		default:
			ASSERT(0);
		}
		skip += cnt;
		if (skip == iov->iov_len) {
			skip = 0;
			uio->uio_iov = (++iov);
			uio->uio_iovcnt--;
		}
		uio->uio_skip = skip;
		uio->uio_resid -= cnt;
		uio->uio_loffset += cnt;
		p = (caddr_t)p + cnt;
		n -= cnt;
	}
	return (0);
}

static int
zfs_uiomove_bvec(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	const struct bio_vec *bv = uio->uio_bvec;
	size_t skip = uio->uio_skip;
	ulong_t cnt;

	while (n && uio->uio_resid) {
		void *paddr;
		cnt = MIN(bv->bv_len - skip, n);

		paddr = zfs_kmap_atomic(bv->bv_page);
		if (rw == UIO_READ)
			bcopy(p, paddr + bv->bv_offset + skip, cnt);
		else
			bcopy(paddr + bv->bv_offset + skip, p, cnt);
		zfs_kunmap_atomic(paddr);

		skip += cnt;
		if (skip == bv->bv_len) {
			skip = 0;
			uio->uio_bvec = (++bv);
			uio->uio_iovcnt--;
		}
		uio->uio_skip = skip;
		uio->uio_resid -= cnt;
		uio->uio_loffset += cnt;
		p = (caddr_t)p + cnt;
		n -= cnt;
	}
	return (0);
}

#if defined(HAVE_VFS_IOV_ITER)
static int
zfs_uiomove_iter(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio,
    boolean_t revert)
{
	size_t cnt = MIN(n, uio->uio_resid);

	if (uio->uio_skip)
		iov_iter_advance(uio->uio_iter, uio->uio_skip);

	if (rw == UIO_READ)
		cnt = copy_to_iter(p, cnt, uio->uio_iter);
	else
		cnt = copy_from_iter(p, cnt, uio->uio_iter);

	/*
	 * When operating on a full pipe no bytes are processed.
	 * In which case return EFAULT which is converted to EAGAIN
	 * by the kernel's generic_file_splice_read() function.
	 */
	if (cnt == 0)
		return (EFAULT);

	/*
	 * Revert advancing the uio_iter.  This is set by zfs_uiocopy()
	 * to avoid consuming the uio and its iov_iter structure.
	 */
	if (revert)
		iov_iter_revert(uio->uio_iter, cnt);

	uio->uio_resid -= cnt;
	uio->uio_loffset += cnt;

	return (0);
}
#endif

int
zfs_uiomove(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	if (uio->uio_segflg == UIO_BVEC)
		return (zfs_uiomove_bvec(p, n, rw, uio));
#if defined(HAVE_VFS_IOV_ITER)
	else if (uio->uio_segflg == UIO_ITER)
		return (zfs_uiomove_iter(p, n, rw, uio, B_FALSE));
#endif
	else
		return (zfs_uiomove_iov(p, n, rw, uio));
}
EXPORT_SYMBOL(zfs_uiomove);

/*
 * Fault in the pages of the first n bytes specified by the uio structure.
 * 1 byte in each page is touched and the uio struct is unmodified. Any
 * error will terminate the process as this is only a best attempt to get
 * the pages resident.
 */
int
zfs_uio_prefaultpages(ssize_t n, zfs_uio_t *uio)
{
	if (uio->uio_segflg == UIO_SYSSPACE || uio->uio_segflg == UIO_BVEC) {
		/* There's never a need to fault in kernel pages */
		return (0);
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		/*
		 * At least a Linux 4.9 kernel, iov_iter_fault_in_readable()
		 * can be relied on to fault in user pages when referenced.
		 */
		if (iov_iter_fault_in_readable(uio->uio_iter, n))
			return (EFAULT);
#endif
	} else {
		/* Fault in all user pages */
		ASSERT3S(uio->uio_segflg, ==, UIO_USERSPACE);
		const struct iovec *iov = uio->uio_iov;
		int iovcnt = uio->uio_iovcnt;
		size_t skip = uio->uio_skip;
		uint8_t tmp;
		caddr_t p;

		for (; n > 0 && iovcnt > 0; iov++, iovcnt--, skip = 0) {
			ulong_t cnt = MIN(iov->iov_len - skip, n);
			/* empty iov */
			if (cnt == 0)
				continue;
			n -= cnt;
			/* touch each page in this segment. */
			p = iov->iov_base + skip;
			while (cnt) {
				if (get_user(tmp, (uint8_t *)p))
					return (EFAULT);
				ulong_t incr = MIN(cnt, PAGESIZE);
				p += incr;
				cnt -= incr;
			}
			/* touch the last byte in case it straddles a page. */
			p--;
			if (get_user(tmp, (uint8_t *)p))
				return (EFAULT);
		}
	}

	return (0);
}
EXPORT_SYMBOL(zfs_uio_prefaultpages);

/*
 * The same as zfs_uiomove() but doesn't modify uio structure.
 * return in cbytes how many bytes were copied.
 */
int
zfs_uiocopy(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio, size_t *cbytes)
{
	zfs_uio_t uio_copy;
	int ret;

	bcopy(uio, &uio_copy, sizeof (zfs_uio_t));

	if (uio->uio_segflg == UIO_BVEC)
		ret = zfs_uiomove_bvec(p, n, rw, &uio_copy);
#if defined(HAVE_VFS_IOV_ITER)
	else if (uio->uio_segflg == UIO_ITER)
		ret = zfs_uiomove_iter(p, n, rw, &uio_copy, B_TRUE);
#endif
	else
		ret = zfs_uiomove_iov(p, n, rw, &uio_copy);

	*cbytes = uio->uio_resid - uio_copy.uio_resid;

	return (ret);
}
EXPORT_SYMBOL(zfs_uiocopy);

/*
 * Drop the next n chars out of *uio.
 */
void
zfs_uioskip(zfs_uio_t *uio, size_t n)
{
	if (n > uio->uio_resid)
		return;

	if (uio->uio_segflg == UIO_BVEC) {
		uio->uio_skip += n;
		while (uio->uio_iovcnt &&
		    uio->uio_skip >= uio->uio_bvec->bv_len) {
			uio->uio_skip -= uio->uio_bvec->bv_len;
			uio->uio_bvec++;
			uio->uio_iovcnt--;
		}
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		iov_iter_advance(uio->uio_iter, n);
#endif
	} else {
		uio->uio_skip += n;
		while (uio->uio_iovcnt &&
		    uio->uio_skip >= uio->uio_iov->iov_len) {
			uio->uio_skip -= uio->uio_iov->iov_len;
			uio->uio_iov++;
			uio->uio_iovcnt--;
		}
	}

	uio->uio_loffset += n;
	uio->uio_resid -= n;
}
EXPORT_SYMBOL(zfs_uioskip);

/*
 * Check if the uio is page-aligned in memory.
 */
boolean_t
zfs_uio_page_aligned(zfs_uio_t *uio)
{
	const struct iovec *iov;

	if (uio->uio_segflg == UIO_USERSPACE ||
	    uio->uio_segflg == UIO_SYSSPACE) {
		iov = uio->uio_iov;
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		iov = uio->uio_iter->iov;
#endif
	} else {
		/* Currently not supported */
		return (B_FALSE);
	}

	size_t skip = uio->uio_skip;

	for (int i = uio->uio_iovcnt; i > 0; iov++, i--) {
		unsigned long addr = (unsigned long)(iov->iov_base + skip);
		size_t size = iov->iov_len - skip;
		if ((addr & (PAGE_SIZE - 1)) ||
		    (size & (PAGE_SIZE - 1))) {
			return (B_FALSE);
		}
		skip = 0;
	}

	return (B_TRUE);
}

#define	ZFS_MARKED_PAGE ((unsigned long)0x5a465350414745) /* ASCII: ZFSPAGE */
#define	IS_ZFS_MARKED_PAGE(_p) \
	(page_private(_p) == (unsigned long)ZFS_MARKED_PAGE)


/*
 * There needs to be care when grabbing the page to either mark it in
 * writeback and write protecting it for Direct IO writes or just calling
 * put_page() for both Direct IO read and writes. A page may either be:
 *  1. A compound page (transparent huge page/huge page)
 *  2. A regular page
 *  3. A regular page that is merged so all other pages are pointing at the
 *     same page because all page contents are identical.
 * In order to handle these cases the private page field is used to mark the
 * once it has been seen. This is necessary for the following reasons:
 *  1. If it is a compound page only the head page needs to used
 *  2. If it is a regular merged page all pages point at the same page
 * Handling of compound page and merged page make sure there is no deadlock by
 * only handling a single page that may represent multiple pages.
 */
static struct page *
zfs_uio_dio_get_page(zfs_uio_t *uio, int curr_page, boolean_t releasing)
{
	struct page *p = uio->uio_dio.pages[curr_page];

	ASSERT3P(p, !=, NULL);

	/*
	 * If this is a compound page, only the head is needed.
	 */
	if (PageCompound(p)) {
		p = compound_head(p);
	}

	lock_page(p);

	if (releasing == B_FALSE && IS_ZFS_MARKED_PAGE(p)) {
		zfs_dbgmsg("curr_page = %d, uio = %p, p = %p, "
		    "zfs_uio_offset(uio) = %lu, "
		    "zfs_uio_resid(uio) = %lu, "
		    "PageKsm(p) = %d, "
		    "p->mapping & PAGE_MAPPING_ANON = %d, "
		    "p->mapping & PAGE_MAPPING_MOVABLE = %d, "
		    "PageAnon(p) = %d, "
		    "PageSwapCache(p) = %d, "
		    "PageKsm(p) = %d, "
		    "PageSlab(p) = %d, "
		    "PageCompound(p) = %d",
		    curr_page, uio, p,
		    (unsigned long)zfs_uio_offset(uio),
		    (unsigned long)zfs_uio_resid(uio),
		    PageKsm(p),
		    page_mapping(p) ? ((unsigned long)p->mapping & PAGE_MAPPING_ANON) != 0: -1,
		    page_mapping(p) ? ((unsigned long)p->mapping & PAGE_MAPPING_MOVABLE) != 0: -1,
		    PageAnon(p),
		    PageSwapCache(p),
		    PageKsm(p),
		    PageSlab(p),
		    PageCompound(p));	
	}

	/*
	 * If this page has already been marked and it is being not released
	 * (see zfs_uio_free_dio_pages()) then the page has already been
	 * set to writeback and write protected. If this is page is being
	 * released but is no longer marked, then the page has already been
	 * removed from writeback (if a write) and does not need to be put
	 * for either read or writes.
	 */
	if ((IS_ZFS_MARKED_PAGE(p) && releasing == B_FALSE) ||
	    (!IS_ZFS_MARKED_PAGE(p) && releasing == B_TRUE)) {
		unlock_page(p);
		return (NULL);
	}

	return (p);
}

static void
zfs_uio_mark_pages(zfs_uio_t *uio)
{
	ASSERT3P(uio->uio_dio.pages, !=, NULL);

	for (int i = 0; i < uio->uio_dio.npages; i++) {
		struct page *p = zfs_uio_dio_get_page(uio, i, B_FALSE);
		if (p == NULL)
			continue;

		ASSERT(PageLocked(p));
		ASSERT(!IS_ZFS_MARKED_PAGE(p));
		SetPagePrivate(p);
		set_page_private(p, (unsigned long)ZFS_MARKED_PAGE);
		unlock_page(p);
	}
}

static void
zfs_uio_set_pages_to_stable(zfs_uio_t *uio)
{
	/*
	 * In order to make the pages stable, we need to lock each page and
	 * check the PG_writeback bit. If the page is under writeback, we
	 * wait till a prior write on the page has finished which is signaled
	 * by end_page_writeback() in zfs_uio_release_stable_pages(). The
	 * page's PTE must also be put under write protection with
	 * clear_page_dirty_for_io().
	 */
	ASSERT3P(uio->uio_dio.pages, !=, NULL);

	for (int i = 0; i < uio->uio_dio.npages; i++) {
		struct page *p = zfs_uio_dio_get_page(uio, i, B_FALSE);
		if (p == NULL)
			continue;

		/*
		 * zfs_uio_dio_get_page() returns the page locked.
		 */
		ASSERT(PageLocked(p));

		while (PageWriteback(p)) {
#ifdef HAVE_PAGEMAP_FOLIO_WAIT_BIT
			folio_wait_bit(page_folio(p), PG_writeback);
#else
			wait_on_page_bit(p, PG_writeback);
#endif
		}

		struct vm_area_struct *vma =
		    find_vma(current->mm, page_to_phys(p));

		zfs_dbgmsg("i = %d, uio = %p, p = %p, "
		    "zfs_uio_offset(uio) = %lu, "
		    "zfs_uio_resid(uio) = %lu, "
		    "page_mapping(p) = %p, "
		    "page_mapping(p) != NULL = %d, "
		    "p->mapping = %p, "
		    "p->mapping != NULL = %d, "
		    "PageAnon(p) = %d, "
		    "PageSwapCache(p) = %d, "
		    "NULL = %p, "
		    "PageKsm(p) = %d, "
		    "PageSlab(p) = %d, "
		    "PageCompound(p) = %d, "
		    "vma->vm_flags & VM_MERGEABLE = %d",
		    i, uio, p,
		    (unsigned long)zfs_uio_offset(uio),
		    (unsigned long)zfs_uio_resid(uio),
		    page_mapping(p),
		    page_mapping(p) != NULL,
		    p->mapping,
		    p->mapping != NULL,
		    PageAnon(p),
		    PageSwapCache(p),
		    NULL,
		    PageKsm(p),
		    PageSlab(p),
		    PageCompound(p),
		    vma ? vma->vm_flags & VM_MERGEABLE ? 1 : 0 : -1);

		if (page_mapping(p)) {
			struct address_space *mapping = page_mapping(p);
			zfs_dbgmsg("i = %d, uio = %p, p = %p, "
			    "mapping->host = %p",
			    i, uio, p, mapping->host);
		}

		clear_page_dirty_for_io(p);
		set_page_writeback(p);

		ASSERT(!IS_ZFS_MARKED_PAGE(p));
		SetPagePrivate(p);
		set_page_private(p, (unsigned long)ZFS_MARKED_PAGE);
		unlock_page(p);
	}
}

void
zfs_uio_free_dio_pages(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	ASSERT(uio->uio_extflg & UIO_DIRECT);
	ASSERT3P(uio->uio_dio.pages, !=, NULL);

	for (int i = 0; i < uio->uio_dio.npages; i++) {
		struct page *p = zfs_uio_dio_get_page(uio, i, B_TRUE);
		if (p == NULL)
			continue;

		/*
		 * zfs_uio_dio_get_page() returns the page locked.
		 */
		ASSERT(PageLocked(p));

		ASSERT(IS_ZFS_MARKED_PAGE(p));
		set_page_private(p, 0);
		ClearPagePrivate(p);

		/*
		 * If this was a Direct IO write we must remove the page from
		 * writeback as that is used to make the page stable
		 * (see comment in zfs_uio_set_pages_to_stable()).
		 */
		if (rw == UIO_WRITE) {
			ASSERT(PageWriteback(p));
			end_page_writeback(p);
		}
		unlock_page(p);
		put_page(p);
	}

	vmem_free(uio->uio_dio.pages,
	    uio->uio_dio.npages * sizeof (struct page *));
}
EXPORT_SYMBOL(zfs_uio_free_dio_pages);

/*
 * zfs_uio_iov_step() is just a modified version of the STEP function of Linux's
 * iov_iter_get_pages().
 */
static size_t
zfs_uio_iov_step(struct iovec v, zfs_uio_rw_t rw, zfs_uio_t *uio, int *numpages)
{
	unsigned long addr = (unsigned long)(v.iov_base);
	size_t len = v.iov_len;
	int n = DIV_ROUND_UP(len, PAGE_SIZE);

	int res = zfs_get_user_pages(P2ALIGN(addr, PAGE_SIZE), n,
	    rw == UIO_READ, &uio->uio_dio.pages[uio->uio_dio.npages]);
	if (res < 0) {
		*numpages = -1;
		return (-res);
	} else if (len != (res * PAGE_SIZE)) {
		*numpages = -1;
		return (len);
	}

	ASSERT3S(len, ==, res * PAGE_SIZE);
	*numpages = res;
	return (len);
}

static int
zfs_uio_get_dio_pages_iov(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	const struct iovec *iovp = uio->uio_iov;
	size_t skip = uio->uio_skip;
	size_t wanted, maxsize;

	ASSERT(uio->uio_segflg != UIO_SYSSPACE);
	wanted = maxsize = uio->uio_resid - skip;

	for (int i = 0; i < uio->uio_iovcnt; i++) {
		struct iovec iov;
		int numpages = 0;

		if (iovp->iov_len == 0) {
			iovp++;
			skip = 0;
			continue;
		}
		iov.iov_len = MIN(maxsize, iovp->iov_len - skip);
		iov.iov_base = iovp->iov_base + skip;
		ssize_t left = zfs_uio_iov_step(iov, rw, uio, &numpages);

		if (numpages == -1) {
			return (left);
		}

		ASSERT3U(left, ==, iov.iov_len);
		uio->uio_dio.npages += numpages;
		maxsize -= iov.iov_len;
		wanted -= left;
		skip = 0;
		iovp++;
	}

	ASSERT0(wanted);
	return (0);
}

#if defined(HAVE_VFS_IOV_ITER)
static int
zfs_uio_get_dio_pages_iov_iter(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	size_t skip = uio->uio_skip;
	size_t wanted = uio->uio_resid - uio->uio_skip;
	size_t rollback = 0;
	size_t cnt;
	size_t maxpages = DIV_ROUND_UP(wanted, PAGE_SIZE);

	while (wanted) {
		cnt = iov_iter_get_pages(uio->uio_iter,
		    &uio->uio_dio.pages[uio->uio_dio.npages],
		    wanted, maxpages, &skip);
		if (cnt < 0) {
			iov_iter_revert(uio->uio_iter, rollback);
			return (SET_ERROR(-cnt));
		}
		uio->uio_dio.npages += DIV_ROUND_UP(cnt, PAGE_SIZE);
		rollback += cnt;
		wanted -= cnt;
		skip = 0;
		iov_iter_advance(uio->uio_iter, cnt);

	}
	ASSERT3U(rollback, ==, uio->uio_resid - uio->uio_skip);
	iov_iter_revert(uio->uio_iter, rollback);

	return (0);
}
#endif /* HAVE_VFS_IOV_ITER */

/*
 * This function maps user pages into the kernel. In the event that the user
 * pages were not mapped successfully an error value is returned.
 *
 * On success, 0 is returned.
 */
int
zfs_uio_get_dio_pages_alloc(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	int error = 0;
	size_t npages = DIV_ROUND_UP(uio->uio_resid, PAGE_SIZE);
	size_t size = npages * sizeof (struct page *);

	if (uio->uio_segflg == UIO_USERSPACE) {
		uio->uio_dio.pages = vmem_alloc(size, KM_SLEEP);
		error = zfs_uio_get_dio_pages_iov(uio, rw);
		ASSERT3S(uio->uio_dio.npages, ==, npages);
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		uio->uio_dio.pages = vmem_alloc(size, KM_SLEEP);
		error = zfs_uio_get_dio_pages_iov_iter(uio, rw);
		ASSERT3S(uio->uio_dio.npages, ==, npages);
#endif
	} else {
		return (SET_ERROR(EOPNOTSUPP));
	}

	if (error) {
		vmem_free(uio->uio_dio.pages, size);
		return (error);
	}

	/*
	 * Since we will be writing the user pages we must make sure that
	 * they are stable. That way the contents of the pages can not change
	 * while we are doing: compression, checksumming, encryption, parity
	 * calculations or deduplication.
	 */
	if (rw == UIO_WRITE)
		zfs_uio_set_pages_to_stable(uio);
	else
		zfs_uio_mark_pages(uio);

	uio->uio_extflg |= UIO_DIRECT;

	return (0);
}

#endif /* _KERNEL */
