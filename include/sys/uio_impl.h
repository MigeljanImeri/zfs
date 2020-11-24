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
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
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

#ifndef _SYS_UIO_IMPL_H
#define	_SYS_UIO_IMPL_H

#include <sys/uio.h>

extern int zfs_uiomove(void *, size_t, zfs_uio_rw_t, zfs_uio_t *);
extern int zfs_uio_prefaultpages(ssize_t, zfs_uio_t *);
extern int zfs_uiocopy(void *, size_t, zfs_uio_rw_t, zfs_uio_t *, size_t *);
extern void zfs_uioskip(zfs_uio_t *, size_t);
extern void zfs_uio_free_dio_pages(zfs_uio_t *, zfs_uio_rw_t);
extern int zfs_uio_get_dio_pages_alloc(zfs_uio_t *, zfs_uio_rw_t);
void zfs_uio_dio_get_offset_pages_cnt(zfs_uio_t *, offset_t *, uint_t *,
    size_t *, uint64_t);
extern boolean_t zfs_uio_page_aligned(zfs_uio_t *);

#endif	/* _SYS_UIO_IMPL_H */
