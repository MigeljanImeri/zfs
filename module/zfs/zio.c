// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011, 2022 by Delphix. All rights reserved.
 * Copyright (c) 2011 Nexenta Systems, Inc. All rights reserved.
 * Copyright (c) 2017, Intel Corporation.
 * Copyright (c) 2019, 2023, 2024, 2025, Klara, Inc.
 * Copyright (c) 2019, Allan Jude
 * Copyright (c) 2021, Datto, Inc.
 * Copyright (c) 2021, 2024 by George Melikov. All rights reserved.
 */

#include <sys/sysmacros.h>
#include <sys/zfs_context.h>
#include <sys/fm/fs/zfs.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_trim.h>
#include <sys/zio_impl.h>
#include <sys/zio_compress.h>
#include <sys/zio_checksum.h>
#include <sys/dmu_objset.h>
#include <sys/arc.h>
#include <sys/brt.h>
#include <sys/ddt.h>
#include <sys/blkptr.h>
#include <sys/zfeature.h>
#include <sys/dsl_scan.h>
#include <sys/metaslab_impl.h>
#include <sys/time.h>
#include <sys/trace_zfs.h>
#include <sys/abd.h>
#include <sys/dsl_crypt.h>
#include <cityhash.h>

/*
 * ==========================================================================
 * I/O type descriptions
 * ==========================================================================
 */
const char *const zio_type_name[ZIO_TYPES] = {
	/*
	 * Note: Linux kernel thread name length is limited
	 * so these names will differ from upstream open zfs.
	 */
	"z_null", "z_rd", "z_wr", "z_fr", "z_cl", "z_flush", "z_trim"
};

int zio_dva_throttle_enabled = B_TRUE;
static int zio_deadman_log_all = B_FALSE;

/*
 * ==========================================================================
 * I/O kmem caches
 * ==========================================================================
 */
static kmem_cache_t *zio_cache;
static kmem_cache_t *zio_link_cache;
kmem_cache_t *zio_buf_cache[SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT];
kmem_cache_t *zio_data_buf_cache[SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT];
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
static uint64_t zio_buf_cache_allocs[SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT];
static uint64_t zio_buf_cache_frees[SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT];
#endif

/* Mark IOs as "slow" if they take longer than 30 seconds */
static uint_t zio_slow_io_ms = (30 * MILLISEC);

#define	BP_SPANB(indblkshift, level) \
	(((uint64_t)1) << ((level) * ((indblkshift) - SPA_BLKPTRSHIFT)))
#define	COMPARE_META_LEVEL	0x80000000ul
/*
 * The following actions directly effect the spa's sync-to-convergence logic.
 * The values below define the sync pass when we start performing the action.
 * Care should be taken when changing these values as they directly impact
 * spa_sync() performance. Tuning these values may introduce subtle performance
 * pathologies and should only be done in the context of performance analysis.
 * These tunables will eventually be removed and replaced with #defines once
 * enough analysis has been done to determine optimal values.
 *
 * The 'zfs_sync_pass_deferred_free' pass must be greater than 1 to ensure that
 * regular blocks are not deferred.
 *
 * Starting in sync pass 8 (zfs_sync_pass_dont_compress), we disable
 * compression (including of metadata).  In practice, we don't have this
 * many sync passes, so this has no effect.
 *
 * The original intent was that disabling compression would help the sync
 * passes to converge. However, in practice disabling compression increases
 * the average number of sync passes, because when we turn compression off, a
 * lot of block's size will change and thus we have to re-allocate (not
 * overwrite) them. It also increases the number of 128KB allocations (e.g.
 * for indirect blocks and spacemaps) because these will not be compressed.
 * The 128K allocations are especially detrimental to performance on highly
 * fragmented systems, which may have very few free segments of this size,
 * and may need to load new metaslabs to satisfy 128K allocations.
 */

/* defer frees starting in this pass */
uint_t zfs_sync_pass_deferred_free = 2;

/* don't compress starting in this pass */
static uint_t zfs_sync_pass_dont_compress = 8;

/* rewrite new bps starting in this pass */
static uint_t zfs_sync_pass_rewrite = 2;

/*
 * An allocating zio is one that either currently has the DVA allocate
 * stage set or will have it later in its lifetime.
 */
#define	IO_IS_ALLOCATING(zio) ((zio)->io_orig_pipeline & ZIO_STAGE_DVA_ALLOCATE)

/*
 * Enable smaller cores by excluding metadata
 * allocations as well.
 */
int zio_exclude_metadata = 0;
static int zio_requeue_io_start_cut_in_line = 1;

#ifdef ZFS_DEBUG
static const int zio_buf_debug_limit = 16384;
#else
static const int zio_buf_debug_limit = 0;
#endif

typedef struct zio_stats {
	kstat_named_t ziostat_total_allocations;
	kstat_named_t ziostat_alloc_class_fallbacks;
	kstat_named_t ziostat_gang_writes;
	kstat_named_t ziostat_gang_multilevel;
} zio_stats_t;

static zio_stats_t zio_stats = {
	{ "total_allocations",	KSTAT_DATA_UINT64 },
	{ "alloc_class_fallbacks",	KSTAT_DATA_UINT64 },
	{ "gang_writes",	KSTAT_DATA_UINT64 },
	{ "gang_multilevel",	KSTAT_DATA_UINT64 },
};

struct {
	wmsum_t ziostat_total_allocations;
	wmsum_t ziostat_alloc_class_fallbacks;
	wmsum_t ziostat_gang_writes;
	wmsum_t ziostat_gang_multilevel;
} ziostat_sums;

#define	ZIOSTAT_BUMP(stat)	wmsum_add(&ziostat_sums.stat, 1);

static kstat_t *zio_ksp;

static inline void __zio_execute(zio_t *zio);

static void zio_taskq_dispatch(zio_t *, zio_taskq_type_t, boolean_t);

static int
zio_kstats_update(kstat_t *ksp, int rw)
{
	zio_stats_t *zs = ksp->ks_data;
	if (rw == KSTAT_WRITE)
		return (EACCES);

	zs->ziostat_total_allocations.value.ui64 =
	    wmsum_value(&ziostat_sums.ziostat_total_allocations);
	zs->ziostat_alloc_class_fallbacks.value.ui64 =
	    wmsum_value(&ziostat_sums.ziostat_alloc_class_fallbacks);
	zs->ziostat_gang_writes.value.ui64 =
	    wmsum_value(&ziostat_sums.ziostat_gang_writes);
	zs->ziostat_gang_multilevel.value.ui64 =
	    wmsum_value(&ziostat_sums.ziostat_gang_multilevel);
	return (0);
}

void
zio_init(void)
{
	size_t c;

	zio_cache = kmem_cache_create("zio_cache",
	    sizeof (zio_t), 0, NULL, NULL, NULL, NULL, NULL, 0);
	zio_link_cache = kmem_cache_create("zio_link_cache",
	    sizeof (zio_link_t), 0, NULL, NULL, NULL, NULL, NULL, 0);

	wmsum_init(&ziostat_sums.ziostat_total_allocations, 0);
	wmsum_init(&ziostat_sums.ziostat_alloc_class_fallbacks, 0);
	wmsum_init(&ziostat_sums.ziostat_gang_writes, 0);
	wmsum_init(&ziostat_sums.ziostat_gang_multilevel, 0);
	zio_ksp = kstat_create("zfs", 0, "zio_stats",
	    "misc", KSTAT_TYPE_NAMED, sizeof (zio_stats) /
	    sizeof (kstat_named_t), KSTAT_FLAG_VIRTUAL);
	if (zio_ksp != NULL) {
		zio_ksp->ks_data = &zio_stats;
		zio_ksp->ks_update = zio_kstats_update;
		kstat_install(zio_ksp);
	}

	for (c = 0; c < SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT; c++) {
		size_t size = (c + 1) << SPA_MINBLOCKSHIFT;
		size_t align, cflags, data_cflags;
		char name[32];

		/*
		 * Create cache for each half-power of 2 size, starting from
		 * SPA_MINBLOCKSIZE.  It should give us memory space efficiency
		 * of ~7/8, sufficient for transient allocations mostly using
		 * these caches.
		 */
		size_t p2 = size;
		while (!ISP2(p2))
			p2 &= p2 - 1;
		if (!IS_P2ALIGNED(size, p2 / 2))
			continue;

#ifndef _KERNEL
		/*
		 * If we are using watchpoints, put each buffer on its own page,
		 * to eliminate the performance overhead of trapping to the
		 * kernel when modifying a non-watched buffer that shares the
		 * page with a watched buffer.
		 */
		if (arc_watch && !IS_P2ALIGNED(size, PAGESIZE))
			continue;
#endif

		if (IS_P2ALIGNED(size, PAGESIZE))
			align = PAGESIZE;
		else
			align = 1 << (highbit64(size ^ (size - 1)) - 1);

		cflags = (zio_exclude_metadata || size > zio_buf_debug_limit) ?
		    KMC_NODEBUG : 0;
		data_cflags = KMC_NODEBUG;
		if (abd_size_alloc_linear(size)) {
			cflags |= KMC_RECLAIMABLE;
			data_cflags |= KMC_RECLAIMABLE;
		}
		if (cflags == data_cflags) {
			/*
			 * Resulting kmem caches would be identical.
			 * Save memory by creating only one.
			 */
			(void) snprintf(name, sizeof (name),
			    "zio_buf_comb_%lu", (ulong_t)size);
			zio_buf_cache[c] = kmem_cache_create(name, size, align,
			    NULL, NULL, NULL, NULL, NULL, cflags);
			zio_data_buf_cache[c] = zio_buf_cache[c];
			continue;
		}
		(void) snprintf(name, sizeof (name), "zio_buf_%lu",
		    (ulong_t)size);
		zio_buf_cache[c] = kmem_cache_create(name, size, align,
		    NULL, NULL, NULL, NULL, NULL, cflags);

		(void) snprintf(name, sizeof (name), "zio_data_buf_%lu",
		    (ulong_t)size);
		zio_data_buf_cache[c] = kmem_cache_create(name, size, align,
		    NULL, NULL, NULL, NULL, NULL, data_cflags);
	}

	while (--c != 0) {
		ASSERT(zio_buf_cache[c] != NULL);
		if (zio_buf_cache[c - 1] == NULL)
			zio_buf_cache[c - 1] = zio_buf_cache[c];

		ASSERT(zio_data_buf_cache[c] != NULL);
		if (zio_data_buf_cache[c - 1] == NULL)
			zio_data_buf_cache[c - 1] = zio_data_buf_cache[c];
	}

	zio_inject_init();

	lz4_init();
}

void
zio_fini(void)
{
	size_t n = SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT;

#if defined(ZFS_DEBUG) && !defined(_KERNEL)
	for (size_t i = 0; i < n; i++) {
		if (zio_buf_cache_allocs[i] != zio_buf_cache_frees[i])
			(void) printf("zio_fini: [%d] %llu != %llu\n",
			    (int)((i + 1) << SPA_MINBLOCKSHIFT),
			    (long long unsigned)zio_buf_cache_allocs[i],
			    (long long unsigned)zio_buf_cache_frees[i]);
	}
#endif

	/*
	 * The same kmem cache can show up multiple times in both zio_buf_cache
	 * and zio_data_buf_cache. Do a wasteful but trivially correct scan to
	 * sort it out.
	 */
	for (size_t i = 0; i < n; i++) {
		kmem_cache_t *cache = zio_buf_cache[i];
		if (cache == NULL)
			continue;
		for (size_t j = i; j < n; j++) {
			if (cache == zio_buf_cache[j])
				zio_buf_cache[j] = NULL;
			if (cache == zio_data_buf_cache[j])
				zio_data_buf_cache[j] = NULL;
		}
		kmem_cache_destroy(cache);
	}

	for (size_t i = 0; i < n; i++) {
		kmem_cache_t *cache = zio_data_buf_cache[i];
		if (cache == NULL)
			continue;
		for (size_t j = i; j < n; j++) {
			if (cache == zio_data_buf_cache[j])
				zio_data_buf_cache[j] = NULL;
		}
		kmem_cache_destroy(cache);
	}

	for (size_t i = 0; i < n; i++) {
		VERIFY3P(zio_buf_cache[i], ==, NULL);
		VERIFY3P(zio_data_buf_cache[i], ==, NULL);
	}

	if (zio_ksp != NULL) {
		kstat_delete(zio_ksp);
		zio_ksp = NULL;
	}

	wmsum_fini(&ziostat_sums.ziostat_total_allocations);
	wmsum_fini(&ziostat_sums.ziostat_alloc_class_fallbacks);
	wmsum_fini(&ziostat_sums.ziostat_gang_writes);
	wmsum_fini(&ziostat_sums.ziostat_gang_multilevel);

	kmem_cache_destroy(zio_link_cache);
	kmem_cache_destroy(zio_cache);

	zio_inject_fini();

	lz4_fini();
}

/*
 * ==========================================================================
 * Allocate and free I/O buffers
 * ==========================================================================
 */

#if defined(ZFS_DEBUG) && defined(_KERNEL)
#define	ZFS_ZIO_BUF_CANARY	1
#endif

#ifdef ZFS_ZIO_BUF_CANARY
static const ulong_t zio_buf_canary = (ulong_t)0xdeadc0dedead210b;

/*
 * Use empty space after the buffer to detect overflows.
 *
 * Since zio_init() creates kmem caches only for certain set of buffer sizes,
 * allocations of different sizes may have some unused space after the data.
 * Filling part of that space with a known pattern on allocation and checking
 * it on free should allow us to detect some buffer overflows.
 */
static void
zio_buf_put_canary(ulong_t *p, size_t size, kmem_cache_t **cache, size_t c)
{
	size_t off = P2ROUNDUP(size, sizeof (ulong_t));
	ulong_t *canary = p + off / sizeof (ulong_t);
	size_t asize = (c + 1) << SPA_MINBLOCKSHIFT;
	if (c + 1 < SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT &&
	    cache[c] == cache[c + 1])
		asize = (c + 2) << SPA_MINBLOCKSHIFT;
	for (; off < asize; canary++, off += sizeof (ulong_t))
		*canary = zio_buf_canary;
}

static void
zio_buf_check_canary(ulong_t *p, size_t size, kmem_cache_t **cache, size_t c)
{
	size_t off = P2ROUNDUP(size, sizeof (ulong_t));
	ulong_t *canary = p + off / sizeof (ulong_t);
	size_t asize = (c + 1) << SPA_MINBLOCKSHIFT;
	if (c + 1 < SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT &&
	    cache[c] == cache[c + 1])
		asize = (c + 2) << SPA_MINBLOCKSHIFT;
	for (; off < asize; canary++, off += sizeof (ulong_t)) {
		if (unlikely(*canary != zio_buf_canary)) {
			PANIC("ZIO buffer overflow %p (%zu) + %zu %#lx != %#lx",
			    p, size, (canary - p) * sizeof (ulong_t),
			    *canary, zio_buf_canary);
		}
	}
}
#endif

/*
 * Use zio_buf_alloc to allocate ZFS metadata.  This data will appear in a
 * crashdump if the kernel panics, so use it judiciously.  Obviously, it's
 * useful to inspect ZFS metadata, but if possible, we should avoid keeping
 * excess / transient data in-core during a crashdump.
 */
void *
zio_buf_alloc(size_t size)
{
	size_t c = (size - 1) >> SPA_MINBLOCKSHIFT;

	VERIFY3U(c, <, SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT);
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
	atomic_add_64(&zio_buf_cache_allocs[c], 1);
#endif

	void *p = kmem_cache_alloc(zio_buf_cache[c], KM_PUSHPAGE);
#ifdef ZFS_ZIO_BUF_CANARY
	zio_buf_put_canary(p, size, zio_buf_cache, c);
#endif
	return (p);
}

/*
 * Use zio_data_buf_alloc to allocate data.  The data will not appear in a
 * crashdump if the kernel panics.  This exists so that we will limit the amount
 * of ZFS data that shows up in a kernel crashdump.  (Thus reducing the amount
 * of kernel heap dumped to disk when the kernel panics)
 */
void *
zio_data_buf_alloc(size_t size)
{
	size_t c = (size - 1) >> SPA_MINBLOCKSHIFT;

	VERIFY3U(c, <, SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT);

	void *p = kmem_cache_alloc(zio_data_buf_cache[c], KM_PUSHPAGE);
#ifdef ZFS_ZIO_BUF_CANARY
	zio_buf_put_canary(p, size, zio_data_buf_cache, c);
#endif
	return (p);
}

void
zio_buf_free(void *buf, size_t size)
{
	size_t c = (size - 1) >> SPA_MINBLOCKSHIFT;

	VERIFY3U(c, <, SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT);
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
	atomic_add_64(&zio_buf_cache_frees[c], 1);
#endif

#ifdef ZFS_ZIO_BUF_CANARY
	zio_buf_check_canary(buf, size, zio_buf_cache, c);
#endif
	kmem_cache_free(zio_buf_cache[c], buf);
}

void
zio_data_buf_free(void *buf, size_t size)
{
	size_t c = (size - 1) >> SPA_MINBLOCKSHIFT;

	VERIFY3U(c, <, SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT);

#ifdef ZFS_ZIO_BUF_CANARY
	zio_buf_check_canary(buf, size, zio_data_buf_cache, c);
#endif
	kmem_cache_free(zio_data_buf_cache[c], buf);
}

static void
zio_abd_free(void *abd, size_t size)
{
	(void) size;
	abd_free((abd_t *)abd);
}

/*
 * ==========================================================================
 * Push and pop I/O transform buffers
 * ==========================================================================
 */
void
zio_push_transform(zio_t *zio, abd_t *data, uint64_t size, uint64_t bufsize,
    zio_transform_func_t *transform)
{
	zio_transform_t *zt = kmem_alloc(sizeof (zio_transform_t), KM_SLEEP);

	zt->zt_orig_abd = zio->io_abd;
	zt->zt_orig_size = zio->io_size;
	zt->zt_bufsize = bufsize;
	zt->zt_transform = transform;

	zt->zt_next = zio->io_transform_stack;
	zio->io_transform_stack = zt;

	zio->io_abd = data;
	zio->io_size = size;
}

void
zio_pop_transforms(zio_t *zio)
{
	zio_transform_t *zt;

	while ((zt = zio->io_transform_stack) != NULL) {
		if (zt->zt_transform != NULL)
			zt->zt_transform(zio,
			    zt->zt_orig_abd, zt->zt_orig_size);

		if (zt->zt_bufsize != 0)
			abd_free(zio->io_abd);

		zio->io_abd = zt->zt_orig_abd;
		zio->io_size = zt->zt_orig_size;
		zio->io_transform_stack = zt->zt_next;

		kmem_free(zt, sizeof (zio_transform_t));
	}
}

/*
 * ==========================================================================
 * I/O transform callbacks for subblocks, decompression, and decryption
 * ==========================================================================
 */
static void
zio_subblock(zio_t *zio, abd_t *data, uint64_t size)
{
	ASSERT(zio->io_size > size);

	if (zio->io_type == ZIO_TYPE_READ)
		abd_copy(data, zio->io_abd, size);
}

static void
zio_decompress(zio_t *zio, abd_t *data, uint64_t size)
{
	if (zio->io_error == 0) {
		int ret = zio_decompress_data(BP_GET_COMPRESS(zio->io_bp),
		    zio->io_abd, data, zio->io_size, size,
		    &zio->io_prop.zp_complevel);

		if (zio_injection_enabled && ret == 0)
			ret = zio_handle_fault_injection(zio, EINVAL);

		if (ret != 0)
			zio->io_error = SET_ERROR(EIO);
	}
}

static void
zio_decrypt(zio_t *zio, abd_t *data, uint64_t size)
{
	int ret;
	void *tmp;
	blkptr_t *bp = zio->io_bp;
	spa_t *spa = zio->io_spa;
	uint64_t dsobj = zio->io_bookmark.zb_objset;
	uint64_t lsize = BP_GET_LSIZE(bp);
	dmu_object_type_t ot = BP_GET_TYPE(bp);
	uint8_t salt[ZIO_DATA_SALT_LEN];
	uint8_t iv[ZIO_DATA_IV_LEN];
	uint8_t mac[ZIO_DATA_MAC_LEN];
	boolean_t no_crypt = B_FALSE;

	ASSERT(BP_USES_CRYPT(bp));
	ASSERT3U(size, !=, 0);

	if (zio->io_error != 0)
		return;

	/*
	 * Verify the cksum of MACs stored in an indirect bp. It will always
	 * be possible to verify this since it does not require an encryption
	 * key.
	 */
	if (BP_HAS_INDIRECT_MAC_CKSUM(bp)) {
		zio_crypt_decode_mac_bp(bp, mac);

		if (BP_GET_COMPRESS(bp) != ZIO_COMPRESS_OFF) {
			/*
			 * We haven't decompressed the data yet, but
			 * zio_crypt_do_indirect_mac_checksum() requires
			 * decompressed data to be able to parse out the MACs
			 * from the indirect block. We decompress it now and
			 * throw away the result after we are finished.
			 */
			abd_t *abd = abd_alloc_linear(lsize, B_TRUE);
			ret = zio_decompress_data(BP_GET_COMPRESS(bp),
			    zio->io_abd, abd, zio->io_size, lsize,
			    &zio->io_prop.zp_complevel);
			if (ret != 0) {
				abd_free(abd);
				ret = SET_ERROR(EIO);
				goto error;
			}
			ret = zio_crypt_do_indirect_mac_checksum_abd(B_FALSE,
			    abd, lsize, BP_SHOULD_BYTESWAP(bp), mac);
			abd_free(abd);
		} else {
			ret = zio_crypt_do_indirect_mac_checksum_abd(B_FALSE,
			    zio->io_abd, size, BP_SHOULD_BYTESWAP(bp), mac);
		}
		abd_copy(data, zio->io_abd, size);

		if (zio_injection_enabled && ot != DMU_OT_DNODE && ret == 0) {
			ret = zio_handle_decrypt_injection(spa,
			    &zio->io_bookmark, ot, ECKSUM);
		}
		if (ret != 0)
			goto error;

		return;
	}

	/*
	 * If this is an authenticated block, just check the MAC. It would be
	 * nice to separate this out into its own flag, but when this was done,
	 * we had run out of bits in what is now zio_flag_t. Future cleanup
	 * could make this a flag bit.
	 */
	if (BP_IS_AUTHENTICATED(bp)) {
		if (ot == DMU_OT_OBJSET) {
			ret = spa_do_crypt_objset_mac_abd(B_FALSE, spa,
			    dsobj, zio->io_abd, size, BP_SHOULD_BYTESWAP(bp));
		} else {
			zio_crypt_decode_mac_bp(bp, mac);
			ret = spa_do_crypt_mac_abd(B_FALSE, spa, dsobj,
			    zio->io_abd, size, mac);
			if (zio_injection_enabled && ret == 0) {
				ret = zio_handle_decrypt_injection(spa,
				    &zio->io_bookmark, ot, ECKSUM);
			}
		}
		abd_copy(data, zio->io_abd, size);

		if (ret != 0)
			goto error;

		return;
	}

	zio_crypt_decode_params_bp(bp, salt, iv);

	if (ot == DMU_OT_INTENT_LOG) {
		tmp = abd_borrow_buf_copy(zio->io_abd, sizeof (zil_chain_t));
		zio_crypt_decode_mac_zil(tmp, mac);
		abd_return_buf(zio->io_abd, tmp, sizeof (zil_chain_t));
	} else {
		zio_crypt_decode_mac_bp(bp, mac);
	}

	ret = spa_do_crypt_abd(B_FALSE, spa, &zio->io_bookmark, BP_GET_TYPE(bp),
	    BP_GET_DEDUP(bp), BP_SHOULD_BYTESWAP(bp), salt, iv, mac, size, data,
	    zio->io_abd, &no_crypt);
	if (no_crypt)
		abd_copy(data, zio->io_abd, size);

	if (ret != 0)
		goto error;

	return;

error:
	/* assert that the key was found unless this was speculative */
	ASSERT(ret != EACCES || (zio->io_flags & ZIO_FLAG_SPECULATIVE));

	/*
	 * If there was a decryption / authentication error return EIO as
	 * the io_error. If this was not a speculative zio, create an ereport.
	 */
	if (ret == ECKSUM) {
		zio->io_error = SET_ERROR(EIO);
		if ((zio->io_flags & ZIO_FLAG_SPECULATIVE) == 0) {
			spa_log_error(spa, &zio->io_bookmark,
			    BP_GET_LOGICAL_BIRTH(zio->io_bp));
			(void) zfs_ereport_post(FM_EREPORT_ZFS_AUTHENTICATION,
			    spa, NULL, &zio->io_bookmark, zio, 0);
		}
	} else {
		zio->io_error = ret;
	}
}

/*
 * ==========================================================================
 * I/O parent/child relationships and pipeline interlocks
 * ==========================================================================
 */
zio_t *
zio_walk_parents(zio_t *cio, zio_link_t **zl)
{
	list_t *pl = &cio->io_parent_list;

	*zl = (*zl == NULL) ? list_head(pl) : list_next(pl, *zl);
	if (*zl == NULL)
		return (NULL);

	ASSERT((*zl)->zl_child == cio);
	return ((*zl)->zl_parent);
}

zio_t *
zio_walk_children(zio_t *pio, zio_link_t **zl)
{
	list_t *cl = &pio->io_child_list;

	ASSERT(MUTEX_HELD(&pio->io_lock));

	*zl = (*zl == NULL) ? list_head(cl) : list_next(cl, *zl);
	if (*zl == NULL)
		return (NULL);

	ASSERT((*zl)->zl_parent == pio);
	return ((*zl)->zl_child);
}

zio_t *
zio_unique_parent(zio_t *cio)
{
	zio_link_t *zl = NULL;
	zio_t *pio = zio_walk_parents(cio, &zl);

	VERIFY3P(zio_walk_parents(cio, &zl), ==, NULL);
	return (pio);
}

static void
zio_add_child_impl(zio_t *pio, zio_t *cio, boolean_t first)
{
	/*
	 * Logical I/Os can have logical, gang, or vdev children.
	 * Gang I/Os can have gang or vdev children.
	 * Vdev I/Os can only have vdev children.
	 * The following ASSERT captures all of these constraints.
	 */
	ASSERT3S(cio->io_child_type, <=, pio->io_child_type);

	/* Parent should not have READY stage if child doesn't have it. */
	IMPLY((cio->io_pipeline & ZIO_STAGE_READY) == 0 &&
	    (cio->io_child_type != ZIO_CHILD_VDEV),
	    (pio->io_pipeline & ZIO_STAGE_READY) == 0);

	zio_link_t *zl = kmem_cache_alloc(zio_link_cache, KM_SLEEP);
	zl->zl_parent = pio;
	zl->zl_child = cio;

	mutex_enter(&pio->io_lock);

	if (first)
		ASSERT(list_is_empty(&cio->io_parent_list));
	else
		mutex_enter(&cio->io_lock);

	ASSERT(pio->io_state[ZIO_WAIT_DONE] == 0);

	uint64_t *countp = pio->io_children[cio->io_child_type];
	for (int w = 0; w < ZIO_WAIT_TYPES; w++)
		countp[w] += !cio->io_state[w];

	list_insert_head(&pio->io_child_list, zl);
	list_insert_head(&cio->io_parent_list, zl);

	if (!first)
		mutex_exit(&cio->io_lock);

	mutex_exit(&pio->io_lock);
}

void
zio_add_child(zio_t *pio, zio_t *cio)
{
	zio_add_child_impl(pio, cio, B_FALSE);
}

static void
zio_add_child_first(zio_t *pio, zio_t *cio)
{
	zio_add_child_impl(pio, cio, B_TRUE);
}

static void
zio_remove_child(zio_t *pio, zio_t *cio, zio_link_t *zl)
{
	ASSERT(zl->zl_parent == pio);
	ASSERT(zl->zl_child == cio);

	mutex_enter(&pio->io_lock);
	mutex_enter(&cio->io_lock);

	list_remove(&pio->io_child_list, zl);
	list_remove(&cio->io_parent_list, zl);

	mutex_exit(&cio->io_lock);
	mutex_exit(&pio->io_lock);
	kmem_cache_free(zio_link_cache, zl);
}

static boolean_t
zio_wait_for_children(zio_t *zio, uint8_t childbits, enum zio_wait_type wait)
{
	boolean_t waiting = B_FALSE;

	mutex_enter(&zio->io_lock);
	ASSERT(zio->io_stall == NULL);
	for (int c = 0; c < ZIO_CHILD_TYPES; c++) {
		if (!(ZIO_CHILD_BIT_IS_SET(childbits, c)))
			continue;

		uint64_t *countp = &zio->io_children[c][wait];
		if (*countp != 0) {
			zio->io_stage >>= 1;
			ASSERT3U(zio->io_stage, !=, ZIO_STAGE_OPEN);
			zio->io_stall = countp;
			waiting = B_TRUE;
			break;
		}
	}
	mutex_exit(&zio->io_lock);
	return (waiting);
}

__attribute__((always_inline))
static inline void
zio_notify_parent(zio_t *pio, zio_t *zio, enum zio_wait_type wait,
    zio_t **next_to_executep)
{
	uint64_t *countp = &pio->io_children[zio->io_child_type][wait];
	int *errorp = &pio->io_child_error[zio->io_child_type];

	mutex_enter(&pio->io_lock);
	if (zio->io_error && !(zio->io_flags & ZIO_FLAG_DONT_PROPAGATE))
		*errorp = zio_worst_error(*errorp, zio->io_error);
	pio->io_post |= zio->io_post;
	ASSERT3U(*countp, >, 0);

	(*countp)--;

	if (*countp == 0 && pio->io_stall == countp) {
		zio_taskq_type_t type =
		    pio->io_stage < ZIO_STAGE_VDEV_IO_START ? ZIO_TASKQ_ISSUE :
		    ZIO_TASKQ_INTERRUPT;
		pio->io_stall = NULL;
		mutex_exit(&pio->io_lock);

		/*
		 * If we can tell the caller to execute this parent next, do
		 * so. We do this if the parent's zio type matches the child's
		 * type, or if it's a zio_null() with no done callback, and so
		 * has no actual work to do. Otherwise dispatch the parent zio
		 * in its own taskq.
		 *
		 * Having the caller execute the parent when possible reduces
		 * locking on the zio taskq's, reduces context switch
		 * overhead, and has no recursion penalty.  Note that one
		 * read from disk typically causes at least 3 zio's: a
		 * zio_null(), the logical zio_read(), and then a physical
		 * zio.  When the physical ZIO completes, we are able to call
		 * zio_done() on all 3 of these zio's from one invocation of
		 * zio_execute() by returning the parent back to
		 * zio_execute().  Since the parent isn't executed until this
		 * thread returns back to zio_execute(), the caller should do
		 * so promptly.
		 *
		 * In other cases, dispatching the parent prevents
		 * overflowing the stack when we have deeply nested
		 * parent-child relationships, as we do with the "mega zio"
		 * of writes for spa_sync(), and the chain of ZIL blocks.
		 */
		if (next_to_executep != NULL && *next_to_executep == NULL &&
		    (pio->io_type == zio->io_type ||
		    (pio->io_type == ZIO_TYPE_NULL && !pio->io_done))) {
			*next_to_executep = pio;
		} else {
			zio_taskq_dispatch(pio, type, B_FALSE);
		}
	} else {
		mutex_exit(&pio->io_lock);
	}
}

static void
zio_inherit_child_errors(zio_t *zio, enum zio_child c)
{
	if (zio->io_child_error[c] != 0 && zio->io_error == 0)
		zio->io_error = zio->io_child_error[c];
}

int
zio_bookmark_compare(const void *x1, const void *x2)
{
	const zio_t *z1 = x1;
	const zio_t *z2 = x2;

	if (z1->io_bookmark.zb_objset < z2->io_bookmark.zb_objset)
		return (-1);
	if (z1->io_bookmark.zb_objset > z2->io_bookmark.zb_objset)
		return (1);

	if (z1->io_bookmark.zb_object < z2->io_bookmark.zb_object)
		return (-1);
	if (z1->io_bookmark.zb_object > z2->io_bookmark.zb_object)
		return (1);

	if (z1->io_bookmark.zb_level < z2->io_bookmark.zb_level)
		return (-1);
	if (z1->io_bookmark.zb_level > z2->io_bookmark.zb_level)
		return (1);

	if (z1->io_bookmark.zb_blkid < z2->io_bookmark.zb_blkid)
		return (-1);
	if (z1->io_bookmark.zb_blkid > z2->io_bookmark.zb_blkid)
		return (1);

	if (z1 < z2)
		return (-1);
	if (z1 > z2)
		return (1);

	return (0);
}

/*
 * ==========================================================================
 * Create the various types of I/O (read, write, free, etc)
 * ==========================================================================
 */
static zio_t *
zio_create(zio_t *pio, spa_t *spa, uint64_t txg, const blkptr_t *bp,
    abd_t *data, uint64_t lsize, uint64_t psize, zio_done_func_t *done,
    void *private, zio_type_t type, zio_priority_t priority,
    zio_flag_t flags, vdev_t *vd, uint64_t offset,
    const zbookmark_phys_t *zb, enum zio_stage stage,
    enum zio_stage pipeline)
{
	zio_t *zio;

	IMPLY(type != ZIO_TYPE_TRIM, psize <= SPA_MAXBLOCKSIZE);
	ASSERT(P2PHASE(psize, SPA_MINBLOCKSIZE) == 0);
	ASSERT(P2PHASE(offset, SPA_MINBLOCKSIZE) == 0);

	ASSERT(!vd || spa_config_held(spa, SCL_STATE_ALL, RW_READER));
	ASSERT(!bp || !(flags & ZIO_FLAG_CONFIG_WRITER));
	ASSERT(vd || stage == ZIO_STAGE_OPEN);

	IMPLY(lsize != psize, (flags & ZIO_FLAG_RAW_COMPRESS) != 0);

	zio = kmem_cache_alloc(zio_cache, KM_SLEEP);
	memset(zio, 0, sizeof (zio_t));

	mutex_init(&zio->io_lock, NULL, MUTEX_NOLOCKDEP, NULL);
	cv_init(&zio->io_cv, NULL, CV_DEFAULT, NULL);

	list_create(&zio->io_parent_list, sizeof (zio_link_t),
	    offsetof(zio_link_t, zl_parent_node));
	list_create(&zio->io_child_list, sizeof (zio_link_t),
	    offsetof(zio_link_t, zl_child_node));
	metaslab_trace_init(&zio->io_alloc_list);

	if (vd != NULL)
		zio->io_child_type = ZIO_CHILD_VDEV;
	else if (flags & ZIO_FLAG_GANG_CHILD)
		zio->io_child_type = ZIO_CHILD_GANG;
	else if (flags & ZIO_FLAG_DDT_CHILD)
		zio->io_child_type = ZIO_CHILD_DDT;
	else
		zio->io_child_type = ZIO_CHILD_LOGICAL;

	if (bp != NULL) {
		if (type != ZIO_TYPE_WRITE ||
		    zio->io_child_type == ZIO_CHILD_DDT) {
			zio->io_bp_copy = *bp;
			zio->io_bp = &zio->io_bp_copy;	/* so caller can free */
		} else {
			zio->io_bp = (blkptr_t *)bp;
		}
		zio->io_bp_orig = *bp;
		if (zio->io_child_type == ZIO_CHILD_LOGICAL)
			zio->io_logical = zio;
		if (zio->io_child_type > ZIO_CHILD_GANG && BP_IS_GANG(bp))
			pipeline |= ZIO_GANG_STAGES;
		if (flags & ZIO_FLAG_PREALLOCATED) {
			BP_ZERO_DVAS(zio->io_bp);
			BP_SET_BIRTH(zio->io_bp, 0, 0);
		}
	}

	zio->io_spa = spa;
	zio->io_txg = txg;
	zio->io_done = done;
	zio->io_private = private;
	zio->io_type = type;
	zio->io_priority = priority;
	zio->io_vd = vd;
	zio->io_offset = offset;
	zio->io_orig_abd = zio->io_abd = data;
	zio->io_orig_size = zio->io_size = psize;
	zio->io_lsize = lsize;
	zio->io_orig_flags = zio->io_flags = flags;
	zio->io_orig_stage = zio->io_stage = stage;
	zio->io_orig_pipeline = zio->io_pipeline = pipeline;
	zio->io_pipeline_trace = ZIO_STAGE_OPEN;
	zio->io_allocator = ZIO_ALLOCATOR_NONE;

	zio->io_state[ZIO_WAIT_READY] = (stage >= ZIO_STAGE_READY) ||
	    (pipeline & ZIO_STAGE_READY) == 0;
	zio->io_state[ZIO_WAIT_DONE] = (stage >= ZIO_STAGE_DONE);

	if (zb != NULL)
		zio->io_bookmark = *zb;

	if (pio != NULL) {
		zio->io_metaslab_class = pio->io_metaslab_class;
		if (zio->io_logical == NULL)
			zio->io_logical = pio->io_logical;
		if (zio->io_child_type == ZIO_CHILD_GANG)
			zio->io_gang_leader = pio->io_gang_leader;
		zio_add_child_first(pio, zio);
	}

	taskq_init_ent(&zio->io_tqent);

	return (zio);
}

void
zio_destroy(zio_t *zio)
{
	metaslab_trace_fini(&zio->io_alloc_list);
	list_destroy(&zio->io_parent_list);
	list_destroy(&zio->io_child_list);
	mutex_destroy(&zio->io_lock);
	cv_destroy(&zio->io_cv);
	kmem_cache_free(zio_cache, zio);
}

/*
 * ZIO intended to be between others.  Provides synchronization at READY
 * and DONE pipeline stages and calls the respective callbacks.
 */
zio_t *
zio_null(zio_t *pio, spa_t *spa, vdev_t *vd, zio_done_func_t *done,
    void *private, zio_flag_t flags)
{
	zio_t *zio;

	zio = zio_create(pio, spa, 0, NULL, NULL, 0, 0, done, private,
	    ZIO_TYPE_NULL, ZIO_PRIORITY_NOW, flags, vd, 0, NULL,
	    ZIO_STAGE_OPEN, ZIO_INTERLOCK_PIPELINE);

	return (zio);
}

/*
 * ZIO intended to be a root of a tree.  Unlike null ZIO does not have a
 * READY pipeline stage (is ready on creation), so it should not be used
 * as child of any ZIO that may need waiting for grandchildren READY stage
 * (any other ZIO type).
 */
zio_t *
zio_root(spa_t *spa, zio_done_func_t *done, void *private, zio_flag_t flags)
{
	zio_t *zio;

	zio = zio_create(NULL, spa, 0, NULL, NULL, 0, 0, done, private,
	    ZIO_TYPE_NULL, ZIO_PRIORITY_NOW, flags, NULL, 0, NULL,
	    ZIO_STAGE_OPEN, ZIO_ROOT_PIPELINE);

	return (zio);
}

static int
zfs_blkptr_verify_log(spa_t *spa, const blkptr_t *bp,
    enum blk_verify_flag blk_verify, const char *fmt, ...)
{
	va_list adx;
	char buf[256];

	va_start(adx, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, adx);
	va_end(adx);

	zfs_dbgmsg("bad blkptr at %px: "
	    "DVA[0]=%#llx/%#llx "
	    "DVA[1]=%#llx/%#llx "
	    "DVA[2]=%#llx/%#llx "
	    "prop=%#llx "
	    "pad=%#llx,%#llx "
	    "phys_birth=%#llx "
	    "birth=%#llx "
	    "fill=%#llx "
	    "cksum=%#llx/%#llx/%#llx/%#llx",
	    bp,
	    (long long)bp->blk_dva[0].dva_word[0],
	    (long long)bp->blk_dva[0].dva_word[1],
	    (long long)bp->blk_dva[1].dva_word[0],
	    (long long)bp->blk_dva[1].dva_word[1],
	    (long long)bp->blk_dva[2].dva_word[0],
	    (long long)bp->blk_dva[2].dva_word[1],
	    (long long)bp->blk_prop,
	    (long long)bp->blk_pad[0],
	    (long long)bp->blk_pad[1],
	    (long long)BP_GET_PHYSICAL_BIRTH(bp),
	    (long long)BP_GET_LOGICAL_BIRTH(bp),
	    (long long)bp->blk_fill,
	    (long long)bp->blk_cksum.zc_word[0],
	    (long long)bp->blk_cksum.zc_word[1],
	    (long long)bp->blk_cksum.zc_word[2],
	    (long long)bp->blk_cksum.zc_word[3]);
	switch (blk_verify) {
	case BLK_VERIFY_HALT:
		zfs_panic_recover("%s: %s", spa_name(spa), buf);
		break;
	case BLK_VERIFY_LOG:
		zfs_dbgmsg("%s: %s", spa_name(spa), buf);
		break;
	case BLK_VERIFY_ONLY:
		break;
	}

	return (1);
}

/*
 * Verify the block pointer fields contain reasonable values.  This means
 * it only contains known object types, checksum/compression identifiers,
 * block sizes within the maximum allowed limits, valid DVAs, etc.
 *
 * If everything checks out 0 is returned.  The zfs_blkptr_verify
 * argument controls the behavior when an invalid field is detected.
 *
 * Values for blk_verify_flag:
 *   BLK_VERIFY_ONLY: evaluate the block
 *   BLK_VERIFY_LOG: evaluate the block and log problems
 *   BLK_VERIFY_HALT: call zfs_panic_recover on error
 *
 * Values for blk_config_flag:
 *   BLK_CONFIG_HELD: caller holds SCL_VDEV for writer
 *   BLK_CONFIG_NEEDED: caller holds no config lock, SCL_VDEV will be
 *   obtained for reader
 *   BLK_CONFIG_SKIP: skip checks which require SCL_VDEV, for better
 *   performance
 */
int
zfs_blkptr_verify(spa_t *spa, const blkptr_t *bp,
    enum blk_config_flag blk_config, enum blk_verify_flag blk_verify)
{
	int errors = 0;

	if (unlikely(!DMU_OT_IS_VALID(BP_GET_TYPE(bp)))) {
		errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
		    "blkptr at %px has invalid TYPE %llu",
		    bp, (longlong_t)BP_GET_TYPE(bp));
	}
	if (unlikely(BP_GET_COMPRESS(bp) >= ZIO_COMPRESS_FUNCTIONS)) {
		errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
		    "blkptr at %px has invalid COMPRESS %llu",
		    bp, (longlong_t)BP_GET_COMPRESS(bp));
	}
	if (unlikely(BP_GET_LSIZE(bp) > SPA_MAXBLOCKSIZE)) {
		errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
		    "blkptr at %px has invalid LSIZE %llu",
		    bp, (longlong_t)BP_GET_LSIZE(bp));
	}
	if (BP_IS_EMBEDDED(bp)) {
		if (unlikely(BPE_GET_ETYPE(bp) >= NUM_BP_EMBEDDED_TYPES)) {
			errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
			    "blkptr at %px has invalid ETYPE %llu",
			    bp, (longlong_t)BPE_GET_ETYPE(bp));
		}
		if (unlikely(BPE_GET_PSIZE(bp) > BPE_PAYLOAD_SIZE)) {
			errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
			    "blkptr at %px has invalid PSIZE %llu",
			    bp, (longlong_t)BPE_GET_PSIZE(bp));
		}
		return (errors ? ECKSUM : 0);
	} else if (BP_IS_HOLE(bp)) {
		/*
		 * Holes are allowed (expected, even) to have no DVAs, no
		 * checksum, and no psize.
		 */
		return (errors ? ECKSUM : 0);
	} else if (unlikely(!DVA_IS_VALID(&bp->blk_dva[0]))) {
		/* Non-hole, non-embedded BPs _must_ have at least one DVA */
		errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
		    "blkptr at %px has no valid DVAs", bp);
	}
	if (unlikely(BP_GET_CHECKSUM(bp) >= ZIO_CHECKSUM_FUNCTIONS)) {
		errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
		    "blkptr at %px has invalid CHECKSUM %llu",
		    bp, (longlong_t)BP_GET_CHECKSUM(bp));
	}
	if (unlikely(BP_GET_PSIZE(bp) > SPA_MAXBLOCKSIZE)) {
		errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
		    "blkptr at %px has invalid PSIZE %llu",
		    bp, (longlong_t)BP_GET_PSIZE(bp));
	}

	/*
	 * Do not verify individual DVAs if the config is not trusted. This
	 * will be done once the zio is executed in vdev_mirror_map_alloc.
	 */
	if (unlikely(!spa->spa_trust_config))
		return (errors ? ECKSUM : 0);

	switch (blk_config) {
	case BLK_CONFIG_HELD:
		ASSERT(spa_config_held(spa, SCL_VDEV, RW_WRITER));
		break;
	case BLK_CONFIG_NEEDED:
		spa_config_enter(spa, SCL_VDEV, bp, RW_READER);
		break;
	case BLK_CONFIG_NEEDED_TRY:
		if (!spa_config_tryenter(spa, SCL_VDEV, bp, RW_READER))
			return (EBUSY);
		break;
	case BLK_CONFIG_SKIP:
		return (errors ? ECKSUM : 0);
	default:
		panic("invalid blk_config %u", blk_config);
	}

	/*
	 * Pool-specific checks.
	 *
	 * Note: it would be nice to verify that the logical birth
	 * and physical birth are not too large.  However,
	 * spa_freeze() allows the birth time of log blocks (and
	 * dmu_sync()-ed blocks that are in the log) to be arbitrarily
	 * large.
	 */
	for (int i = 0; i < BP_GET_NDVAS(bp); i++) {
		const dva_t *dva = &bp->blk_dva[i];
		uint64_t vdevid = DVA_GET_VDEV(dva);

		if (unlikely(vdevid >= spa->spa_root_vdev->vdev_children)) {
			errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
			    "blkptr at %px DVA %u has invalid VDEV %llu",
			    bp, i, (longlong_t)vdevid);
			continue;
		}
		vdev_t *vd = spa->spa_root_vdev->vdev_child[vdevid];
		if (unlikely(vd == NULL)) {
			errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
			    "blkptr at %px DVA %u has invalid VDEV %llu",
			    bp, i, (longlong_t)vdevid);
			continue;
		}
		if (unlikely(vd->vdev_ops == &vdev_hole_ops)) {
			errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
			    "blkptr at %px DVA %u has hole VDEV %llu",
			    bp, i, (longlong_t)vdevid);
			continue;
		}
		if (vd->vdev_ops == &vdev_missing_ops) {
			/*
			 * "missing" vdevs are valid during import, but we
			 * don't have their detailed info (e.g. asize), so
			 * we can't perform any more checks on them.
			 */
			continue;
		}
		uint64_t offset = DVA_GET_OFFSET(dva);
		uint64_t asize = DVA_GET_ASIZE(dva);
		if (DVA_GET_GANG(dva))
			asize = vdev_gang_header_asize(vd);
		if (unlikely(offset + asize > vd->vdev_asize)) {
			errors += zfs_blkptr_verify_log(spa, bp, blk_verify,
			    "blkptr at %px DVA %u has invalid OFFSET %llu",
			    bp, i, (longlong_t)offset);
		}
	}
	if (blk_config == BLK_CONFIG_NEEDED || blk_config ==
	    BLK_CONFIG_NEEDED_TRY)
		spa_config_exit(spa, SCL_VDEV, bp);

	return (errors ? ECKSUM : 0);
}

boolean_t
zfs_dva_valid(spa_t *spa, const dva_t *dva, const blkptr_t *bp)
{
	(void) bp;
	uint64_t vdevid = DVA_GET_VDEV(dva);

	if (vdevid >= spa->spa_root_vdev->vdev_children)
		return (B_FALSE);

	vdev_t *vd = spa->spa_root_vdev->vdev_child[vdevid];
	if (vd == NULL)
		return (B_FALSE);

	if (vd->vdev_ops == &vdev_hole_ops)
		return (B_FALSE);

	if (vd->vdev_ops == &vdev_missing_ops) {
		return (B_FALSE);
	}

	uint64_t offset = DVA_GET_OFFSET(dva);
	uint64_t asize = DVA_GET_ASIZE(dva);

	if (DVA_GET_GANG(dva))
		asize = vdev_gang_header_asize(vd);
	if (offset + asize > vd->vdev_asize)
		return (B_FALSE);

	return (B_TRUE);
}

zio_t *
zio_read(zio_t *pio, spa_t *spa, const blkptr_t *bp,
    abd_t *data, uint64_t size, zio_done_func_t *done, void *private,
    zio_priority_t priority, zio_flag_t flags, const zbookmark_phys_t *zb)
{
	zio_t *zio;

	zio = zio_create(pio, spa, BP_GET_BIRTH(bp), bp,
	    data, size, size, done, private,
	    ZIO_TYPE_READ, priority, flags, NULL, 0, zb,
	    ZIO_STAGE_OPEN, (flags & ZIO_FLAG_DDT_CHILD) ?
	    ZIO_DDT_CHILD_READ_PIPELINE : ZIO_READ_PIPELINE);

	return (zio);
}

zio_t *
zio_write(zio_t *pio, spa_t *spa, uint64_t txg, blkptr_t *bp,
    abd_t *data, uint64_t lsize, uint64_t psize, const zio_prop_t *zp,
    zio_done_func_t *ready, zio_done_func_t *children_ready,
    zio_done_func_t *done, void *private, zio_priority_t priority,
    zio_flag_t flags, const zbookmark_phys_t *zb)
{
	zio_t *zio;
	enum zio_stage pipeline = zp->zp_direct_write == B_TRUE ?
	    ZIO_DIRECT_WRITE_PIPELINE : (flags & ZIO_FLAG_DDT_CHILD) ?
	    ZIO_DDT_CHILD_WRITE_PIPELINE : ZIO_WRITE_PIPELINE;


	zio = zio_create(pio, spa, txg, bp, data, lsize, psize, done, private,
	    ZIO_TYPE_WRITE, priority, flags, NULL, 0, zb,
	    ZIO_STAGE_OPEN, pipeline);

	zio->io_ready = ready;
	zio->io_children_ready = children_ready;
	zio->io_prop = *zp;

	/*
	 * Data can be NULL if we are going to call zio_write_override() to
	 * provide the already-allocated BP.  But we may need the data to
	 * verify a dedup hit (if requested).  In this case, don't try to
	 * dedup (just take the already-allocated BP verbatim). Encrypted
	 * dedup blocks need data as well so we also disable dedup in this
	 * case.
	 */
	if (data == NULL &&
	    (zio->io_prop.zp_dedup_verify || zio->io_prop.zp_encrypt)) {
		zio->io_prop.zp_dedup = zio->io_prop.zp_dedup_verify = B_FALSE;
	}

	return (zio);
}

zio_t *
zio_rewrite(zio_t *pio, spa_t *spa, uint64_t txg, blkptr_t *bp, abd_t *data,
    uint64_t size, zio_done_func_t *done, void *private,
    zio_priority_t priority, zio_flag_t flags, zbookmark_phys_t *zb)
{
	zio_t *zio;

	zio = zio_create(pio, spa, txg, bp, data, size, size, done, private,
	    ZIO_TYPE_WRITE, priority, flags | ZIO_FLAG_IO_REWRITE, NULL, 0, zb,
	    ZIO_STAGE_OPEN, ZIO_REWRITE_PIPELINE);

	return (zio);
}

void
zio_write_override(zio_t *zio, blkptr_t *bp, int copies, int gang_copies,
    boolean_t nopwrite, boolean_t brtwrite)
{
	ASSERT(zio->io_type == ZIO_TYPE_WRITE);
	ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);
	ASSERT(zio->io_stage == ZIO_STAGE_OPEN);
	ASSERT(zio->io_txg == spa_syncing_txg(zio->io_spa));
	ASSERT(!brtwrite || !nopwrite);

	/*
	 * We must reset the io_prop to match the values that existed
	 * when the bp was first written by dmu_sync() keeping in mind
	 * that nopwrite and dedup are mutually exclusive.
	 */
	zio->io_prop.zp_dedup = nopwrite ? B_FALSE : zio->io_prop.zp_dedup;
	zio->io_prop.zp_nopwrite = nopwrite;
	zio->io_prop.zp_brtwrite = brtwrite;
	zio->io_prop.zp_copies = copies;
	zio->io_prop.zp_gang_copies = gang_copies;
	zio->io_bp_override = bp;
}

void
zio_free(spa_t *spa, uint64_t txg, const blkptr_t *bp)
{

	(void) zfs_blkptr_verify(spa, bp, BLK_CONFIG_NEEDED, BLK_VERIFY_HALT);

	/*
	 * The check for EMBEDDED is a performance optimization.  We
	 * process the free here (by ignoring it) rather than
	 * putting it on the list and then processing it in zio_free_sync().
	 */
	if (BP_IS_EMBEDDED(bp))
		return;

	/*
	 * Frees that are for the currently-syncing txg, are not going to be
	 * deferred, and which will not need to do a read (i.e. not GANG or
	 * DEDUP), can be processed immediately.  Otherwise, put them on the
	 * in-memory list for later processing.
	 *
	 * Note that we only defer frees after zfs_sync_pass_deferred_free
	 * when the log space map feature is disabled. [see relevant comment
	 * in spa_sync_iterate_to_convergence()]
	 */
	if (BP_IS_GANG(bp) ||
	    BP_GET_DEDUP(bp) ||
	    txg != spa->spa_syncing_txg ||
	    (spa_sync_pass(spa) >= zfs_sync_pass_deferred_free &&
	    !spa_feature_is_active(spa, SPA_FEATURE_LOG_SPACEMAP)) ||
	    brt_maybe_exists(spa, bp)) {
		metaslab_check_free(spa, bp);
		bplist_append(&spa->spa_free_bplist[txg & TXG_MASK], bp);
	} else {
		VERIFY3P(zio_free_sync(NULL, spa, txg, bp, 0), ==, NULL);
	}
}

/*
 * To improve performance, this function may return NULL if we were able
 * to do the free immediately.  This avoids the cost of creating a zio
 * (and linking it to the parent, etc).
 */
zio_t *
zio_free_sync(zio_t *pio, spa_t *spa, uint64_t txg, const blkptr_t *bp,
    zio_flag_t flags)
{
	ASSERT(!BP_IS_HOLE(bp));
	ASSERT(spa_syncing_txg(spa) == txg);

	if (BP_IS_EMBEDDED(bp))
		return (NULL);

	metaslab_check_free(spa, bp);
	arc_freed(spa, bp);
	dsl_scan_freed(spa, bp);

	if (BP_IS_GANG(bp) ||
	    BP_GET_DEDUP(bp) ||
	    brt_maybe_exists(spa, bp)) {
		/*
		 * GANG, DEDUP and BRT blocks can induce a read (for the gang
		 * block header, the DDT or the BRT), so issue them
		 * asynchronously so that this thread is not tied up.
		 */
		enum zio_stage stage =
		    ZIO_FREE_PIPELINE | ZIO_STAGE_ISSUE_ASYNC;

		return (zio_create(pio, spa, txg, bp, NULL, BP_GET_PSIZE(bp),
		    BP_GET_PSIZE(bp), NULL, NULL,
		    ZIO_TYPE_FREE, ZIO_PRIORITY_NOW,
		    flags, NULL, 0, NULL, ZIO_STAGE_OPEN, stage));
	} else {
		metaslab_free(spa, bp, txg, B_FALSE);
		return (NULL);
	}
}

zio_t *
zio_claim(zio_t *pio, spa_t *spa, uint64_t txg, const blkptr_t *bp,
    zio_done_func_t *done, void *private, zio_flag_t flags)
{
	zio_t *zio;

	(void) zfs_blkptr_verify(spa, bp, (flags & ZIO_FLAG_CONFIG_WRITER) ?
	    BLK_CONFIG_HELD : BLK_CONFIG_NEEDED, BLK_VERIFY_HALT);

	if (BP_IS_EMBEDDED(bp))
		return (zio_null(pio, spa, NULL, NULL, NULL, 0));

	/*
	 * A claim is an allocation of a specific block.  Claims are needed
	 * to support immediate writes in the intent log.  The issue is that
	 * immediate writes contain committed data, but in a txg that was
	 * *not* committed.  Upon opening the pool after an unclean shutdown,
	 * the intent log claims all blocks that contain immediate write data
	 * so that the SPA knows they're in use.
	 *
	 * All claims *must* be resolved in the first txg -- before the SPA
	 * starts allocating blocks -- so that nothing is allocated twice.
	 * If txg == 0 we just verify that the block is claimable.
	 */
	ASSERT3U(BP_GET_LOGICAL_BIRTH(&spa->spa_uberblock.ub_rootbp), <,
	    spa_min_claim_txg(spa));
	ASSERT(txg == spa_min_claim_txg(spa) || txg == 0);
	ASSERT(!BP_GET_DEDUP(bp) || !spa_writeable(spa));	/* zdb(8) */

	zio = zio_create(pio, spa, txg, bp, NULL, BP_GET_PSIZE(bp),
	    BP_GET_PSIZE(bp), done, private, ZIO_TYPE_CLAIM, ZIO_PRIORITY_NOW,
	    flags, NULL, 0, NULL, ZIO_STAGE_OPEN, ZIO_CLAIM_PIPELINE);
	ASSERT0(zio->io_queued_timestamp);

	return (zio);
}

zio_t *
zio_trim(zio_t *pio, vdev_t *vd, uint64_t offset, uint64_t size,
    zio_done_func_t *done, void *private, zio_priority_t priority,
    zio_flag_t flags, enum trim_flag trim_flags)
{
	zio_t *zio;

	ASSERT0(vd->vdev_children);
	ASSERT0(P2PHASE(offset, 1ULL << vd->vdev_ashift));
	ASSERT0(P2PHASE(size, 1ULL << vd->vdev_ashift));
	ASSERT3U(size, !=, 0);

	zio = zio_create(pio, vd->vdev_spa, 0, NULL, NULL, size, size, done,
	    private, ZIO_TYPE_TRIM, priority, flags | ZIO_FLAG_PHYSICAL,
	    vd, offset, NULL, ZIO_STAGE_OPEN, ZIO_TRIM_PIPELINE);
	zio->io_trim_flags = trim_flags;

	return (zio);
}

zio_t *
zio_read_phys(zio_t *pio, vdev_t *vd, uint64_t offset, uint64_t size,
    abd_t *data, int checksum, zio_done_func_t *done, void *private,
    zio_priority_t priority, zio_flag_t flags, boolean_t labels)
{
	zio_t *zio;

	ASSERT(vd->vdev_children == 0);
	ASSERT(!labels || offset + size <= VDEV_LABEL_START_SIZE ||
	    offset >= vd->vdev_psize - VDEV_LABEL_END_SIZE);
	ASSERT3U(offset + size, <=, vd->vdev_psize);

	zio = zio_create(pio, vd->vdev_spa, 0, NULL, data, size, size, done,
	    private, ZIO_TYPE_READ, priority, flags | ZIO_FLAG_PHYSICAL, vd,
	    offset, NULL, ZIO_STAGE_OPEN, ZIO_READ_PHYS_PIPELINE);

	zio->io_prop.zp_checksum = checksum;

	return (zio);
}

zio_t *
zio_write_phys(zio_t *pio, vdev_t *vd, uint64_t offset, uint64_t size,
    abd_t *data, int checksum, zio_done_func_t *done, void *private,
    zio_priority_t priority, zio_flag_t flags, boolean_t labels)
{
	zio_t *zio;

	ASSERT(vd->vdev_children == 0);
	ASSERT(!labels || offset + size <= VDEV_LABEL_START_SIZE ||
	    offset >= vd->vdev_psize - VDEV_LABEL_END_SIZE);
	ASSERT3U(offset + size, <=, vd->vdev_psize);

	zio = zio_create(pio, vd->vdev_spa, 0, NULL, data, size, size, done,
	    private, ZIO_TYPE_WRITE, priority, flags | ZIO_FLAG_PHYSICAL, vd,
	    offset, NULL, ZIO_STAGE_OPEN, ZIO_WRITE_PHYS_PIPELINE);

	zio->io_prop.zp_checksum = checksum;

	if (zio_checksum_table[checksum].ci_flags & ZCHECKSUM_FLAG_EMBEDDED) {
		/*
		 * zec checksums are necessarily destructive -- they modify
		 * the end of the write buffer to hold the verifier/checksum.
		 * Therefore, we must make a local copy in case the data is
		 * being written to multiple places in parallel.
		 */
		abd_t *wbuf = abd_alloc_sametype(data, size);
		abd_copy(wbuf, data, size);

		zio_push_transform(zio, wbuf, size, size, NULL);
	}

	return (zio);
}

/*
 * Create a child I/O to do some work for us.
 */
zio_t *
zio_vdev_child_io(zio_t *pio, blkptr_t *bp, vdev_t *vd, uint64_t offset,
    abd_t *data, uint64_t size, int type, zio_priority_t priority,
    zio_flag_t flags, zio_done_func_t *done, void *private)
{
	enum zio_stage pipeline = ZIO_VDEV_CHILD_PIPELINE;
	zio_t *zio;

	/*
	 * vdev child I/Os do not propagate their error to the parent.
	 * Therefore, for correct operation the caller *must* check for
	 * and handle the error in the child i/o's done callback.
	 * The only exceptions are i/os that we don't care about
	 * (OPTIONAL or REPAIR).
	 */
	ASSERT((flags & ZIO_FLAG_OPTIONAL) || (flags & ZIO_FLAG_IO_REPAIR) ||
	    done != NULL);

	if (type == ZIO_TYPE_READ && bp != NULL) {
		/*
		 * If we have the bp, then the child should perform the
		 * checksum and the parent need not.  This pushes error
		 * detection as close to the leaves as possible and
		 * eliminates redundant checksums in the interior nodes.
		 */
		pipeline |= ZIO_STAGE_CHECKSUM_VERIFY;
		pio->io_pipeline &= ~ZIO_STAGE_CHECKSUM_VERIFY;
		/*
		 * We never allow the mirror VDEV to attempt reading from any
		 * additional data copies after the first Direct I/O checksum
		 * verify failure. This is to avoid bad data being written out
		 * through the mirror during self healing. See comment in
		 * vdev_mirror_io_done() for more details.
		 */
		ASSERT0(pio->io_post & ZIO_POST_DIO_CHKSUM_ERR);
	} else if (type == ZIO_TYPE_WRITE &&
	    pio->io_prop.zp_direct_write == B_TRUE) {
		/*
		 * By default we only will verify checksums for Direct I/O
		 * writes for Linux. FreeBSD is able to place user pages under
		 * write protection before issuing them to the ZIO pipeline.
		 *
		 * Checksum validation errors will only be reported through
		 * the top-level VDEV, which is set by this child ZIO.
		 */
		ASSERT3P(bp, !=, NULL);
		ASSERT3U(pio->io_child_type, ==, ZIO_CHILD_LOGICAL);
		pipeline |= ZIO_STAGE_DIO_CHECKSUM_VERIFY;
	}

	if (vd->vdev_ops->vdev_op_leaf) {
		ASSERT0(vd->vdev_children);
		offset += VDEV_LABEL_START_SIZE;
	}

	flags |= ZIO_VDEV_CHILD_FLAGS(pio);

	/*
	 * If we've decided to do a repair, the write is not speculative --
	 * even if the original read was.
	 */
	if (flags & ZIO_FLAG_IO_REPAIR)
		flags &= ~ZIO_FLAG_SPECULATIVE;

	/*
	 * If we're creating a child I/O that is not associated with a
	 * top-level vdev, then the child zio is not an allocating I/O.
	 * If this is a retried I/O then we ignore it since we will
	 * have already processed the original allocating I/O.
	 */
	if (flags & ZIO_FLAG_ALLOC_THROTTLED &&
	    (vd != vd->vdev_top || (flags & ZIO_FLAG_IO_RETRY))) {
		ASSERT(pio->io_metaslab_class != NULL);
		ASSERT(pio->io_metaslab_class->mc_alloc_throttle_enabled);
		ASSERT(type == ZIO_TYPE_WRITE);
		ASSERT(priority == ZIO_PRIORITY_ASYNC_WRITE);
		ASSERT(!(flags & ZIO_FLAG_IO_REPAIR));
		ASSERT(!(pio->io_flags & ZIO_FLAG_IO_REWRITE) ||
		    pio->io_child_type == ZIO_CHILD_GANG);

		flags &= ~ZIO_FLAG_ALLOC_THROTTLED;
	}

	zio = zio_create(pio, pio->io_spa, pio->io_txg, bp, data, size, size,
	    done, private, type, priority, flags, vd, offset, &pio->io_bookmark,
	    ZIO_STAGE_VDEV_IO_START >> 1, pipeline);
	ASSERT3U(zio->io_child_type, ==, ZIO_CHILD_VDEV);

	return (zio);
}

zio_t *
zio_vdev_delegated_io(vdev_t *vd, uint64_t offset, abd_t *data, uint64_t size,
    zio_type_t type, zio_priority_t priority, zio_flag_t flags,
    zio_done_func_t *done, void *private)
{
	zio_t *zio;

	ASSERT(vd->vdev_ops->vdev_op_leaf);

	zio = zio_create(NULL, vd->vdev_spa, 0, NULL,
	    data, size, size, done, private, type, priority,
	    flags | ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_RETRY | ZIO_FLAG_DELEGATED,
	    vd, offset, NULL,
	    ZIO_STAGE_VDEV_IO_START >> 1, ZIO_VDEV_CHILD_PIPELINE);

	return (zio);
}


/*
 * Send a flush command to the given vdev. Unlike most zio creation functions,
 * the flush zios are issued immediately. You can wait on pio to pause until
 * the flushes complete.
 */
void
zio_flush(zio_t *pio, vdev_t *vd)
{
	const zio_flag_t flags = ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_PROPAGATE |
	    ZIO_FLAG_DONT_RETRY;

	if (vd->vdev_nowritecache)
		return;

	if (vd->vdev_children == 0) {
		zio_nowait(zio_create(pio, vd->vdev_spa, 0, NULL, NULL, 0, 0,
		    NULL, NULL, ZIO_TYPE_FLUSH, ZIO_PRIORITY_NOW, flags, vd, 0,
		    NULL, ZIO_STAGE_OPEN, ZIO_FLUSH_PIPELINE));
	} else {
		for (uint64_t c = 0; c < vd->vdev_children; c++)
			zio_flush(pio, vd->vdev_child[c]);
	}
}

void
zio_shrink(zio_t *zio, uint64_t size)
{
	ASSERT3P(zio->io_executor, ==, NULL);
	ASSERT3U(zio->io_orig_size, ==, zio->io_size);
	ASSERT3U(size, <=, zio->io_size);

	/*
	 * We don't shrink for raidz because of problems with the
	 * reconstruction when reading back less than the block size.
	 * Note, BP_IS_RAIDZ() assumes no compression.
	 */
	ASSERT(BP_GET_COMPRESS(zio->io_bp) == ZIO_COMPRESS_OFF);
	if (!BP_IS_RAIDZ(zio->io_bp)) {
		/* we are not doing a raw write */
		ASSERT3U(zio->io_size, ==, zio->io_lsize);
		zio->io_orig_size = zio->io_size = zio->io_lsize = size;
	}
}

/*
 * Round provided allocation size up to a value that can be allocated
 * by at least some vdev(s) in the pool with minimum or no additional
 * padding and without extra space usage on others
 */
static uint64_t
zio_roundup_alloc_size(spa_t *spa, uint64_t size)
{
	if (size > spa->spa_min_alloc)
		return (roundup(size, spa->spa_gcd_alloc));
	return (spa->spa_min_alloc);
}

size_t
zio_get_compression_max_size(enum zio_compress compress, uint64_t gcd_alloc,
    uint64_t min_alloc, size_t s_len)
{
	size_t d_len;

	/* minimum 12.5% must be saved (legacy value, may be changed later) */
	d_len = s_len - (s_len >> 3);

	/* ZLE can't use exactly d_len bytes, it needs more, so ignore it */
	if (compress == ZIO_COMPRESS_ZLE)
		return (d_len);

	d_len = d_len - d_len % gcd_alloc;

	if (d_len < min_alloc)
		return (BPE_PAYLOAD_SIZE);
	return (d_len);
}

/*
 * ==========================================================================
 * Prepare to read and write logical blocks
 * ==========================================================================
 */

static zio_t *
zio_read_bp_init(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	uint64_t psize =
	    BP_IS_EMBEDDED(bp) ? BPE_GET_PSIZE(bp) : BP_GET_PSIZE(bp);

	ASSERT3P(zio->io_bp, ==, &zio->io_bp_copy);

	if (BP_GET_COMPRESS(bp) != ZIO_COMPRESS_OFF &&
	    zio->io_child_type == ZIO_CHILD_LOGICAL &&
	    !(zio->io_flags & ZIO_FLAG_RAW_COMPRESS)) {
		zio_push_transform(zio, abd_alloc_sametype(zio->io_abd, psize),
		    psize, psize, zio_decompress);
	}

	if (((BP_IS_PROTECTED(bp) && !(zio->io_flags & ZIO_FLAG_RAW_ENCRYPT)) ||
	    BP_HAS_INDIRECT_MAC_CKSUM(bp)) &&
	    zio->io_child_type == ZIO_CHILD_LOGICAL) {
		zio_push_transform(zio, abd_alloc_sametype(zio->io_abd, psize),
		    psize, psize, zio_decrypt);
	}

	if (BP_IS_EMBEDDED(bp) && BPE_GET_ETYPE(bp) == BP_EMBEDDED_TYPE_DATA) {
		int psize = BPE_GET_PSIZE(bp);
		void *data = abd_borrow_buf(zio->io_abd, psize);

		zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;
		decode_embedded_bp_compressed(bp, data);
		abd_return_buf_copy(zio->io_abd, data, psize);
	} else {
		ASSERT(!BP_IS_EMBEDDED(bp));
	}

	if (BP_GET_DEDUP(bp) && zio->io_child_type == ZIO_CHILD_LOGICAL)
		zio->io_pipeline = ZIO_DDT_READ_PIPELINE;

	return (zio);
}

static zio_t *
zio_write_bp_init(zio_t *zio)
{
	if (!IO_IS_ALLOCATING(zio))
		return (zio);

	ASSERT(zio->io_child_type != ZIO_CHILD_DDT);

	if (zio->io_bp_override) {
		blkptr_t *bp = zio->io_bp;
		zio_prop_t *zp = &zio->io_prop;

		ASSERT(BP_GET_LOGICAL_BIRTH(bp) != zio->io_txg);

		*bp = *zio->io_bp_override;
		zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;

		if (zp->zp_brtwrite)
			return (zio);

		ASSERT(!BP_GET_DEDUP(zio->io_bp_override));

		if (BP_IS_EMBEDDED(bp))
			return (zio);

		/*
		 * If we've been overridden and nopwrite is set then
		 * set the flag accordingly to indicate that a nopwrite
		 * has already occurred.
		 */
		if (!BP_IS_HOLE(bp) && zp->zp_nopwrite) {
			ASSERT(!zp->zp_dedup);
			ASSERT3U(BP_GET_CHECKSUM(bp), ==, zp->zp_checksum);
			zio->io_flags |= ZIO_FLAG_NOPWRITE;
			return (zio);
		}

		ASSERT(!zp->zp_nopwrite);

		if (BP_IS_HOLE(bp) || !zp->zp_dedup)
			return (zio);

		ASSERT((zio_checksum_table[zp->zp_checksum].ci_flags &
		    ZCHECKSUM_FLAG_DEDUP) || zp->zp_dedup_verify);

		if (BP_GET_CHECKSUM(bp) == zp->zp_checksum &&
		    !zp->zp_encrypt) {
			BP_SET_DEDUP(bp, 1);
			zio->io_pipeline |= ZIO_STAGE_DDT_WRITE;
			return (zio);
		}

		/*
		 * We were unable to handle this as an override bp, treat
		 * it as a regular write I/O.
		 */
		zio->io_bp_override = NULL;
		*bp = zio->io_bp_orig;
		zio->io_pipeline = zio->io_orig_pipeline;
	}

	return (zio);
}

static zio_t *
zio_write_compress(zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	zio_prop_t *zp = &zio->io_prop;
	enum zio_compress compress = zp->zp_compress;
	blkptr_t *bp = zio->io_bp;
	uint64_t lsize = zio->io_lsize;
	uint64_t psize = zio->io_size;
	uint32_t pass = 1;

	/*
	 * If our children haven't all reached the ready stage,
	 * wait for them and then repeat this pipeline stage.
	 */
	if (zio_wait_for_children(zio, ZIO_CHILD_LOGICAL_BIT |
	    ZIO_CHILD_GANG_BIT, ZIO_WAIT_READY)) {
		return (NULL);
	}

	if (!IO_IS_ALLOCATING(zio))
		return (zio);

	if (zio->io_children_ready != NULL) {
		/*
		 * Now that all our children are ready, run the callback
		 * associated with this zio in case it wants to modify the
		 * data to be written.
		 */
		ASSERT3U(zp->zp_level, >, 0);
		zio->io_children_ready(zio);
	}

	ASSERT(zio->io_child_type != ZIO_CHILD_DDT);
	ASSERT(zio->io_bp_override == NULL);

	if (!BP_IS_HOLE(bp) && BP_GET_LOGICAL_BIRTH(bp) == zio->io_txg) {
		/*
		 * We're rewriting an existing block, which means we're
		 * working on behalf of spa_sync().  For spa_sync() to
		 * converge, it must eventually be the case that we don't
		 * have to allocate new blocks.  But compression changes
		 * the blocksize, which forces a reallocate, and makes
		 * convergence take longer.  Therefore, after the first
		 * few passes, stop compressing to ensure convergence.
		 */
		pass = spa_sync_pass(spa);

		ASSERT(zio->io_txg == spa_syncing_txg(spa));
		ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);
		ASSERT(!BP_GET_DEDUP(bp));

		if (pass >= zfs_sync_pass_dont_compress)
			compress = ZIO_COMPRESS_OFF;

		/* Make sure someone doesn't change their mind on overwrites */
		ASSERT(BP_IS_EMBEDDED(bp) || BP_IS_GANG(bp) ||
		    MIN(zp->zp_copies, spa_max_replication(spa))
		    == BP_GET_NDVAS(bp));
	}

	/* If it's a compressed write that is not raw, compress the buffer. */
	if (compress != ZIO_COMPRESS_OFF &&
	    !(zio->io_flags & ZIO_FLAG_RAW_COMPRESS)) {
		abd_t *cabd = NULL;
		if (abd_cmp_zero(zio->io_abd, lsize) == 0)
			psize = 0;
		else if (compress == ZIO_COMPRESS_EMPTY)
			psize = lsize;
		else
			psize = zio_compress_data(compress, zio->io_abd, &cabd,
			    lsize,
			    zio_get_compression_max_size(compress,
			    spa->spa_gcd_alloc, spa->spa_min_alloc, lsize),
			    zp->zp_complevel);
		if (psize == 0) {
			compress = ZIO_COMPRESS_OFF;
		} else if (psize >= lsize) {
			compress = ZIO_COMPRESS_OFF;
			if (cabd != NULL)
				abd_free(cabd);
		} else if (psize <= BPE_PAYLOAD_SIZE && !zp->zp_encrypt &&
		    zp->zp_level == 0 && !DMU_OT_HAS_FILL(zp->zp_type) &&
		    spa_feature_is_enabled(spa, SPA_FEATURE_EMBEDDED_DATA)) {
			void *cbuf = abd_borrow_buf_copy(cabd, lsize);
			encode_embedded_bp_compressed(bp,
			    cbuf, compress, lsize, psize);
			BPE_SET_ETYPE(bp, BP_EMBEDDED_TYPE_DATA);
			BP_SET_TYPE(bp, zio->io_prop.zp_type);
			BP_SET_LEVEL(bp, zio->io_prop.zp_level);
			abd_return_buf(cabd, cbuf, lsize);
			abd_free(cabd);
			BP_SET_LOGICAL_BIRTH(bp, zio->io_txg);
			zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;
			ASSERT(spa_feature_is_active(spa,
			    SPA_FEATURE_EMBEDDED_DATA));
			return (zio);
		} else {
			/*
			 * Round compressed size up to the minimum allocation
			 * size of the smallest-ashift device, and zero the
			 * tail. This ensures that the compressed size of the
			 * BP (and thus compressratio property) are correct,
			 * in that we charge for the padding used to fill out
			 * the last sector.
			 */
			size_t rounded = (size_t)zio_roundup_alloc_size(spa,
			    psize);
			if (rounded >= lsize) {
				compress = ZIO_COMPRESS_OFF;
				abd_free(cabd);
				psize = lsize;
			} else {
				abd_zero_off(cabd, psize, rounded - psize);
				psize = rounded;
				zio_push_transform(zio, cabd,
				    psize, lsize, NULL);
			}
		}

		/*
		 * We were unable to handle this as an override bp, treat
		 * it as a regular write I/O.
		 */
		zio->io_bp_override = NULL;
		*bp = zio->io_bp_orig;
		zio->io_pipeline = zio->io_orig_pipeline;

	} else if ((zio->io_flags & ZIO_FLAG_RAW_ENCRYPT) != 0 &&
	    zp->zp_type == DMU_OT_DNODE) {
		/*
		 * The DMU actually relies on the zio layer's compression
		 * to free metadnode blocks that have had all contained
		 * dnodes freed. As a result, even when doing a raw
		 * receive, we must check whether the block can be compressed
		 * to a hole.
		 */
		if (abd_cmp_zero(zio->io_abd, lsize) == 0) {
			psize = 0;
			compress = ZIO_COMPRESS_OFF;
		} else {
			psize = lsize;
		}
	} else if (zio->io_flags & ZIO_FLAG_RAW_COMPRESS &&
	    !(zio->io_flags & ZIO_FLAG_RAW_ENCRYPT)) {
		/*
		 * If we are raw receiving an encrypted dataset we should not
		 * take this codepath because it will change the on-disk block
		 * and decryption will fail.
		 */
		size_t rounded = MIN((size_t)zio_roundup_alloc_size(spa, psize),
		    lsize);

		if (rounded != psize) {
			abd_t *cdata = abd_alloc_linear(rounded, B_TRUE);
			abd_zero_off(cdata, psize, rounded - psize);
			abd_copy_off(cdata, zio->io_abd, 0, 0, psize);
			psize = rounded;
			zio_push_transform(zio, cdata,
			    psize, rounded, NULL);
		}
	} else {
		ASSERT3U(psize, !=, 0);
	}

	/*
	 * The final pass of spa_sync() must be all rewrites, but the first
	 * few passes offer a trade-off: allocating blocks defers convergence,
	 * but newly allocated blocks are sequential, so they can be written
	 * to disk faster.  Therefore, we allow the first few passes of
	 * spa_sync() to allocate new blocks, but force rewrites after that.
	 * There should only be a handful of blocks after pass 1 in any case.
	 */
	if (!BP_IS_HOLE(bp) && BP_GET_LOGICAL_BIRTH(bp) == zio->io_txg &&
	    BP_GET_PSIZE(bp) == psize &&
	    pass >= zfs_sync_pass_rewrite) {
		VERIFY3U(psize, !=, 0);
		enum zio_stage gang_stages = zio->io_pipeline & ZIO_GANG_STAGES;

		zio->io_pipeline = ZIO_REWRITE_PIPELINE | gang_stages;
		zio->io_flags |= ZIO_FLAG_IO_REWRITE;
	} else {
		BP_ZERO(bp);
		zio->io_pipeline = ZIO_WRITE_PIPELINE;
	}

	if (psize == 0) {
		if (BP_GET_LOGICAL_BIRTH(&zio->io_bp_orig) != 0 &&
		    spa_feature_is_active(spa, SPA_FEATURE_HOLE_BIRTH)) {
			BP_SET_LSIZE(bp, lsize);
			BP_SET_TYPE(bp, zp->zp_type);
			BP_SET_LEVEL(bp, zp->zp_level);
			BP_SET_BIRTH(bp, zio->io_txg, 0);
		}
		zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;
	} else {
		ASSERT(zp->zp_checksum != ZIO_CHECKSUM_GANG_HEADER);
		BP_SET_LSIZE(bp, lsize);
		BP_SET_TYPE(bp, zp->zp_type);
		BP_SET_LEVEL(bp, zp->zp_level);
		BP_SET_PSIZE(bp, psize);
		BP_SET_COMPRESS(bp, compress);
		BP_SET_CHECKSUM(bp, zp->zp_checksum);
		BP_SET_DEDUP(bp, zp->zp_dedup);
		BP_SET_BYTEORDER(bp, ZFS_HOST_BYTEORDER);
		if (zp->zp_dedup) {
			ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);
			ASSERT(!(zio->io_flags & ZIO_FLAG_IO_REWRITE));
			ASSERT(!zp->zp_encrypt ||
			    DMU_OT_IS_ENCRYPTED(zp->zp_type));
			zio->io_pipeline = ZIO_DDT_WRITE_PIPELINE;
		}
		if (zp->zp_nopwrite) {
			ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);
			ASSERT(!(zio->io_flags & ZIO_FLAG_IO_REWRITE));
			zio->io_pipeline |= ZIO_STAGE_NOP_WRITE;
		}
	}
	return (zio);
}

static zio_t *
zio_free_bp_init(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;

	if (zio->io_child_type == ZIO_CHILD_LOGICAL) {
		if (BP_GET_DEDUP(bp))
			zio->io_pipeline = ZIO_DDT_FREE_PIPELINE;
	}

	ASSERT3P(zio->io_bp, ==, &zio->io_bp_copy);

	return (zio);
}

/*
 * ==========================================================================
 * Execute the I/O pipeline
 * ==========================================================================
 */

static void
zio_taskq_dispatch(zio_t *zio, zio_taskq_type_t q, boolean_t cutinline)
{
	spa_t *spa = zio->io_spa;
	zio_type_t t = zio->io_type;

	/*
	 * If we're a config writer or a probe, the normal issue and
	 * interrupt threads may all be blocked waiting for the config lock.
	 * In this case, select the otherwise-unused taskq for ZIO_TYPE_NULL.
	 */
	if (zio->io_flags & (ZIO_FLAG_CONFIG_WRITER | ZIO_FLAG_PROBE))
		t = ZIO_TYPE_NULL;

	/*
	 * A similar issue exists for the L2ARC write thread until L2ARC 2.0.
	 */
	if (t == ZIO_TYPE_WRITE && zio->io_vd && zio->io_vd->vdev_aux)
		t = ZIO_TYPE_NULL;

	/*
	 * If this is a high priority I/O, then use the high priority taskq if
	 * available or cut the line otherwise.
	 */
	if (zio->io_priority == ZIO_PRIORITY_SYNC_WRITE) {
		if (spa->spa_zio_taskq[t][q + 1].stqs_count != 0)
			q++;
		else
			cutinline = B_TRUE;
	}

	ASSERT3U(q, <, ZIO_TASKQ_TYPES);

	spa_taskq_dispatch(spa, t, q, zio_execute, zio, cutinline);
}

static boolean_t
zio_taskq_member(zio_t *zio, zio_taskq_type_t q)
{
	spa_t *spa = zio->io_spa;

	taskq_t *tq = taskq_of_curthread();

	for (zio_type_t t = 0; t < ZIO_TYPES; t++) {
		spa_taskqs_t *tqs = &spa->spa_zio_taskq[t][q];
		uint_t i;
		for (i = 0; i < tqs->stqs_count; i++) {
			if (tqs->stqs_taskq[i] == tq)
				return (B_TRUE);
		}
	}

	return (B_FALSE);
}

static zio_t *
zio_issue_async(zio_t *zio)
{
	ASSERT((zio->io_type != ZIO_TYPE_WRITE) || ZIO_HAS_ALLOCATOR(zio));
	zio_taskq_dispatch(zio, ZIO_TASKQ_ISSUE, B_FALSE);
	return (NULL);
}

void
zio_interrupt(void *zio)
{
	zio_taskq_dispatch(zio, ZIO_TASKQ_INTERRUPT, B_FALSE);
}

void
zio_delay_interrupt(zio_t *zio)
{
	/*
	 * The timeout_generic() function isn't defined in userspace, so
	 * rather than trying to implement the function, the zio delay
	 * functionality has been disabled for userspace builds.
	 */

#ifdef _KERNEL
	/*
	 * If io_target_timestamp is zero, then no delay has been registered
	 * for this IO, thus jump to the end of this function and "skip" the
	 * delay; issuing it directly to the zio layer.
	 */
	if (zio->io_target_timestamp != 0) {
		hrtime_t now = gethrtime();

		if (now >= zio->io_target_timestamp) {
			/*
			 * This IO has already taken longer than the target
			 * delay to complete, so we don't want to delay it
			 * any longer; we "miss" the delay and issue it
			 * directly to the zio layer. This is likely due to
			 * the target latency being set to a value less than
			 * the underlying hardware can satisfy (e.g. delay
			 * set to 1ms, but the disks take 10ms to complete an
			 * IO request).
			 */

			DTRACE_PROBE2(zio__delay__miss, zio_t *, zio,
			    hrtime_t, now);

			zio_interrupt(zio);
		} else {
			taskqid_t tid;
			hrtime_t diff = zio->io_target_timestamp - now;
			int ticks = MAX(1, NSEC_TO_TICK(diff));
			clock_t expire_at_tick = ddi_get_lbolt() + ticks;

			DTRACE_PROBE3(zio__delay__hit, zio_t *, zio,
			    hrtime_t, now, hrtime_t, diff);

			tid = taskq_dispatch_delay(system_taskq, zio_interrupt,
			    zio, TQ_NOSLEEP, expire_at_tick);
			if (tid == TASKQID_INVALID) {
				/*
				 * Couldn't allocate a task.  Just finish the
				 * zio without a delay.
				 */
				zio_interrupt(zio);
			}
		}
		return;
	}
#endif
	DTRACE_PROBE1(zio__delay__skip, zio_t *, zio);
	zio_interrupt(zio);
}

static void
zio_deadman_impl(zio_t *pio, int ziodepth)
{
	zio_t *cio, *cio_next;
	zio_link_t *zl = NULL;
	vdev_t *vd = pio->io_vd;
	uint64_t failmode = spa_get_deadman_failmode(pio->io_spa);

	if (zio_deadman_log_all || (vd != NULL && vd->vdev_ops->vdev_op_leaf)) {
		vdev_queue_t *vq = vd ? &vd->vdev_queue : NULL;
		zbookmark_phys_t *zb = &pio->io_bookmark;
		uint64_t delta = gethrtime() - pio->io_timestamp;

		zfs_dbgmsg("slow zio[%d]: zio=%px timestamp=%llu "
		    "delta=%llu queued=%llu io=%llu "
		    "path=%s "
		    "last=%llu type=%d "
		    "priority=%d flags=0x%llx stage=0x%x "
		    "pipeline=0x%x pipeline-trace=0x%x "
		    "objset=%llu object=%llu "
		    "level=%llu blkid=%llu "
		    "offset=%llu size=%llu "
		    "error=%d",
		    ziodepth, pio, pio->io_timestamp,
		    (u_longlong_t)delta, pio->io_delta, pio->io_delay,
		    vd ? vd->vdev_path : "NULL",
		    vq ? vq->vq_io_complete_ts : 0, pio->io_type,
		    pio->io_priority, (u_longlong_t)pio->io_flags,
		    pio->io_stage, pio->io_pipeline, pio->io_pipeline_trace,
		    (u_longlong_t)zb->zb_objset, (u_longlong_t)zb->zb_object,
		    (u_longlong_t)zb->zb_level, (u_longlong_t)zb->zb_blkid,
		    (u_longlong_t)pio->io_offset, (u_longlong_t)pio->io_size,
		    pio->io_error);
		(void) zfs_ereport_post(FM_EREPORT_ZFS_DEADMAN,
		    pio->io_spa, vd, zb, pio, 0);
	}

	if (vd != NULL && vd->vdev_ops->vdev_op_leaf &&
	    list_is_empty(&pio->io_child_list) &&
	    failmode == ZIO_FAILURE_MODE_CONTINUE &&
	    taskq_empty_ent(&pio->io_tqent) &&
	    pio->io_queue_state == ZIO_QS_ACTIVE) {
		pio->io_error = EINTR;
		zio_interrupt(pio);
	}

	mutex_enter(&pio->io_lock);
	for (cio = zio_walk_children(pio, &zl); cio != NULL; cio = cio_next) {
		cio_next = zio_walk_children(pio, &zl);
		zio_deadman_impl(cio, ziodepth + 1);
	}
	mutex_exit(&pio->io_lock);
}

/*
 * Log the critical information describing this zio and all of its children
 * using the zfs_dbgmsg() interface then post deadman event for the ZED.
 */
void
zio_deadman(zio_t *pio, const char *tag)
{
	spa_t *spa = pio->io_spa;
	char *name = spa_name(spa);

	if (!zfs_deadman_enabled || spa_suspended(spa))
		return;

	zio_deadman_impl(pio, 0);

	switch (spa_get_deadman_failmode(spa)) {
	case ZIO_FAILURE_MODE_WAIT:
		zfs_dbgmsg("%s waiting for hung I/O to pool '%s'", tag, name);
		break;

	case ZIO_FAILURE_MODE_CONTINUE:
		zfs_dbgmsg("%s restarting hung I/O for pool '%s'", tag, name);
		break;

	case ZIO_FAILURE_MODE_PANIC:
		fm_panic("%s determined I/O to pool '%s' is hung.", tag, name);
		break;
	}
}

/*
 * Execute the I/O pipeline until one of the following occurs:
 * (1) the I/O completes; (2) the pipeline stalls waiting for
 * dependent child I/Os; (3) the I/O issues, so we're waiting
 * for an I/O completion interrupt; (4) the I/O is delegated by
 * vdev-level caching or aggregation; (5) the I/O is deferred
 * due to vdev-level queueing; (6) the I/O is handed off to
 * another thread.  In all cases, the pipeline stops whenever
 * there's no CPU work; it never burns a thread in cv_wait_io().
 *
 * There's no locking on io_stage because there's no legitimate way
 * for multiple threads to be attempting to process the same I/O.
 */
static zio_pipe_stage_t *zio_pipeline[];

/*
 * zio_execute() is a wrapper around the static function
 * __zio_execute() so that we can force  __zio_execute() to be
 * inlined.  This reduces stack overhead which is important
 * because __zio_execute() is called recursively in several zio
 * code paths.  zio_execute() itself cannot be inlined because
 * it is externally visible.
 */
void
zio_execute(void *zio)
{
	fstrans_cookie_t cookie;

	cookie = spl_fstrans_mark();
	__zio_execute(zio);
	spl_fstrans_unmark(cookie);
}

/*
 * Used to determine if in the current context the stack is sized large
 * enough to allow zio_execute() to be called recursively.  A minimum
 * stack size of 16K is required to avoid needing to re-dispatch the zio.
 */
static boolean_t
zio_execute_stack_check(zio_t *zio)
{
#if !defined(HAVE_LARGE_STACKS)
	dsl_pool_t *dp = spa_get_dsl(zio->io_spa);

	/* Executing in txg_sync_thread() context. */
	if (dp && curthread == dp->dp_tx.tx_sync_thread)
		return (B_TRUE);

	/* Pool initialization outside of zio_taskq context. */
	if (dp && spa_is_initializing(dp->dp_spa) &&
	    !zio_taskq_member(zio, ZIO_TASKQ_ISSUE) &&
	    !zio_taskq_member(zio, ZIO_TASKQ_ISSUE_HIGH))
		return (B_TRUE);
#else
	(void) zio;
#endif /* HAVE_LARGE_STACKS */

	return (B_FALSE);
}

__attribute__((always_inline))
static inline void
__zio_execute(zio_t *zio)
{
	ASSERT3U(zio->io_queued_timestamp, >, 0);

	while (zio->io_stage < ZIO_STAGE_DONE) {
		enum zio_stage pipeline = zio->io_pipeline;
		enum zio_stage stage = zio->io_stage;

		zio->io_executor = curthread;

		ASSERT(!MUTEX_HELD(&zio->io_lock));
		ASSERT(ISP2(stage));
		ASSERT(zio->io_stall == NULL);

		do {
			stage <<= 1;
		} while ((stage & pipeline) == 0);

		ASSERT(stage <= ZIO_STAGE_DONE);

		/*
		 * If we are in interrupt context and this pipeline stage
		 * will grab a config lock that is held across I/O,
		 * or may wait for an I/O that needs an interrupt thread
		 * to complete, issue async to avoid deadlock.
		 *
		 * For VDEV_IO_START, we cut in line so that the io will
		 * be sent to disk promptly.
		 */
		if ((stage & ZIO_BLOCKING_STAGES) && zio->io_vd == NULL &&
		    zio_taskq_member(zio, ZIO_TASKQ_INTERRUPT)) {
			boolean_t cut = (stage == ZIO_STAGE_VDEV_IO_START) ?
			    zio_requeue_io_start_cut_in_line : B_FALSE;
			zio_taskq_dispatch(zio, ZIO_TASKQ_ISSUE, cut);
			return;
		}

		/*
		 * If the current context doesn't have large enough stacks
		 * the zio must be issued asynchronously to prevent overflow.
		 */
		if (zio_execute_stack_check(zio)) {
			boolean_t cut = (stage == ZIO_STAGE_VDEV_IO_START) ?
			    zio_requeue_io_start_cut_in_line : B_FALSE;
			zio_taskq_dispatch(zio, ZIO_TASKQ_ISSUE, cut);
			return;
		}

		zio->io_stage = stage;
		zio->io_pipeline_trace |= zio->io_stage;

		/*
		 * The zio pipeline stage returns the next zio to execute
		 * (typically the same as this one), or NULL if we should
		 * stop.
		 */
		zio = zio_pipeline[highbit64(stage) - 1](zio);

		if (zio == NULL)
			return;
	}
}


/*
 * ==========================================================================
 * Initiate I/O, either sync or async
 * ==========================================================================
 */
int
zio_wait(zio_t *zio)
{
	/*
	 * Some routines, like zio_free_sync(), may return a NULL zio
	 * to avoid the performance overhead of creating and then destroying
	 * an unneeded zio.  For the callers' simplicity, we accept a NULL
	 * zio and ignore it.
	 */
	if (zio == NULL)
		return (0);

	long timeout = MSEC_TO_TICK(zfs_deadman_ziotime_ms);
	int error;

	ASSERT3S(zio->io_stage, ==, ZIO_STAGE_OPEN);
	ASSERT3P(zio->io_executor, ==, NULL);

	zio->io_waiter = curthread;
	ASSERT0(zio->io_queued_timestamp);
	zio->io_queued_timestamp = gethrtime();

	if (zio->io_type == ZIO_TYPE_WRITE) {
		spa_select_allocator(zio);
	}
	__zio_execute(zio);

	mutex_enter(&zio->io_lock);
	while (zio->io_executor != NULL) {
		error = cv_timedwait_io(&zio->io_cv, &zio->io_lock,
		    ddi_get_lbolt() + timeout);

		if (zfs_deadman_enabled && error == -1 &&
		    gethrtime() - zio->io_queued_timestamp >
		    spa_deadman_ziotime(zio->io_spa)) {
			mutex_exit(&zio->io_lock);
			timeout = MSEC_TO_TICK(zfs_deadman_checktime_ms);
			zio_deadman(zio, FTAG);
			mutex_enter(&zio->io_lock);
		}
	}
	mutex_exit(&zio->io_lock);

	error = zio->io_error;
	zio_destroy(zio);

	return (error);
}

void
zio_nowait(zio_t *zio)
{
	/*
	 * See comment in zio_wait().
	 */
	if (zio == NULL)
		return;

	ASSERT3P(zio->io_executor, ==, NULL);

	if (zio->io_child_type == ZIO_CHILD_LOGICAL &&
	    list_is_empty(&zio->io_parent_list)) {
		zio_t *pio;

		/*
		 * This is a logical async I/O with no parent to wait for it.
		 * We add it to the spa_async_root_zio "Godfather" I/O which
		 * will ensure they complete prior to unloading the pool.
		 */
		spa_t *spa = zio->io_spa;
		pio = spa->spa_async_zio_root[CPU_SEQID_UNSTABLE];

		zio_add_child(pio, zio);
	}

	ASSERT0(zio->io_queued_timestamp);
	zio->io_queued_timestamp = gethrtime();
	if (zio->io_type == ZIO_TYPE_WRITE) {
		spa_select_allocator(zio);
	}
	__zio_execute(zio);
}

/*
 * ==========================================================================
 * Reexecute, cancel, or suspend/resume failed I/O
 * ==========================================================================
 */

static void
zio_reexecute(void *arg)
{
	zio_t *pio = arg;
	zio_t *cio, *cio_next, *gio;

	ASSERT(pio->io_child_type == ZIO_CHILD_LOGICAL);
	ASSERT(pio->io_orig_stage == ZIO_STAGE_OPEN);
	ASSERT(pio->io_gang_leader == NULL);
	ASSERT(pio->io_gang_tree == NULL);

	mutex_enter(&pio->io_lock);
	pio->io_flags = pio->io_orig_flags;
	pio->io_stage = pio->io_orig_stage;
	pio->io_pipeline = pio->io_orig_pipeline;
	pio->io_post = 0;
	pio->io_flags |= ZIO_FLAG_REEXECUTED;
	pio->io_pipeline_trace = 0;
	pio->io_error = 0;
	pio->io_state[ZIO_WAIT_READY] = (pio->io_stage >= ZIO_STAGE_READY) ||
	    (pio->io_pipeline & ZIO_STAGE_READY) == 0;
	pio->io_state[ZIO_WAIT_DONE] = (pio->io_stage >= ZIO_STAGE_DONE);

	/*
	 * It's possible for a failed ZIO to be a descendant of more than one
	 * ZIO tree. When reexecuting it, we have to be sure to add its wait
	 * states to all parent wait counts.
	 *
	 * Those parents, in turn, may have other children that are currently
	 * active, usually because they've already been reexecuted after
	 * resuming. Those children may be executing and may call
	 * zio_notify_parent() at the same time as we're updating our parent's
	 * counts. To avoid races while updating the counts, we take
	 * gio->io_lock before each update.
	 */
	zio_link_t *zl = NULL;
	while ((gio = zio_walk_parents(pio, &zl)) != NULL) {
		mutex_enter(&gio->io_lock);
		for (int w = 0; w < ZIO_WAIT_TYPES; w++) {
			gio->io_children[pio->io_child_type][w] +=
			    !pio->io_state[w];
		}
		mutex_exit(&gio->io_lock);
	}

	for (int c = 0; c < ZIO_CHILD_TYPES; c++)
		pio->io_child_error[c] = 0;

	if (IO_IS_ALLOCATING(pio))
		BP_ZERO(pio->io_bp);

	/*
	 * As we reexecute pio's children, new children could be created.
	 * New children go to the head of pio's io_child_list, however,
	 * so we will (correctly) not reexecute them.  The key is that
	 * the remainder of pio's io_child_list, from 'cio_next' onward,
	 * cannot be affected by any side effects of reexecuting 'cio'.
	 */
	zl = NULL;
	for (cio = zio_walk_children(pio, &zl); cio != NULL; cio = cio_next) {
		cio_next = zio_walk_children(pio, &zl);
		mutex_exit(&pio->io_lock);
		zio_reexecute(cio);
		mutex_enter(&pio->io_lock);
	}
	mutex_exit(&pio->io_lock);

	/*
	 * Now that all children have been reexecuted, execute the parent.
	 * We don't reexecute "The Godfather" I/O here as it's the
	 * responsibility of the caller to wait on it.
	 */
	if (!(pio->io_flags & ZIO_FLAG_GODFATHER)) {
		pio->io_queued_timestamp = gethrtime();
		__zio_execute(pio);
	}
}

void
zio_suspend(spa_t *spa, zio_t *zio, zio_suspend_reason_t reason)
{
	if (spa_get_failmode(spa) == ZIO_FAILURE_MODE_PANIC)
		fm_panic("Pool '%s' has encountered an uncorrectable I/O "
		    "failure and the failure mode property for this pool "
		    "is set to panic.", spa_name(spa));

	if (reason != ZIO_SUSPEND_MMP) {
		cmn_err(CE_WARN, "Pool '%s' has encountered an uncorrectable "
		    "I/O failure and has been suspended.", spa_name(spa));
	}

	(void) zfs_ereport_post(FM_EREPORT_ZFS_IO_FAILURE, spa, NULL,
	    NULL, NULL, 0);

	mutex_enter(&spa->spa_suspend_lock);

	if (spa->spa_suspend_zio_root == NULL)
		spa->spa_suspend_zio_root = zio_root(spa, NULL, NULL,
		    ZIO_FLAG_CANFAIL | ZIO_FLAG_SPECULATIVE |
		    ZIO_FLAG_GODFATHER);

	spa->spa_suspended = reason;

	if (zio != NULL) {
		ASSERT(!(zio->io_flags & ZIO_FLAG_GODFATHER));
		ASSERT(zio != spa->spa_suspend_zio_root);
		ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);
		ASSERT(zio_unique_parent(zio) == NULL);
		ASSERT(zio->io_stage == ZIO_STAGE_DONE);
		zio_add_child(spa->spa_suspend_zio_root, zio);
	}

	mutex_exit(&spa->spa_suspend_lock);

	txg_wait_kick(spa->spa_dsl_pool);
}

int
zio_resume(spa_t *spa)
{
	zio_t *pio;

	/*
	 * Reexecute all previously suspended i/o.
	 */
	mutex_enter(&spa->spa_suspend_lock);
	if (spa->spa_suspended != ZIO_SUSPEND_NONE)
		cmn_err(CE_WARN, "Pool '%s' was suspended and is being "
		    "resumed. Failed I/O will be retried.",
		    spa_name(spa));
	spa->spa_suspended = ZIO_SUSPEND_NONE;
	cv_broadcast(&spa->spa_suspend_cv);
	pio = spa->spa_suspend_zio_root;
	spa->spa_suspend_zio_root = NULL;
	mutex_exit(&spa->spa_suspend_lock);

	if (pio == NULL)
		return (0);

	zio_reexecute(pio);
	return (zio_wait(pio));
}

void
zio_resume_wait(spa_t *spa)
{
	mutex_enter(&spa->spa_suspend_lock);
	while (spa_suspended(spa))
		cv_wait(&spa->spa_suspend_cv, &spa->spa_suspend_lock);
	mutex_exit(&spa->spa_suspend_lock);
}

/*
 * ==========================================================================
 * Gang blocks.
 *
 * A gang block is a collection of small blocks that looks to the DMU
 * like one large block.  When zio_dva_allocate() cannot find a block
 * of the requested size, due to either severe fragmentation or the pool
 * being nearly full, it calls zio_write_gang_block() to construct the
 * block from smaller fragments.
 *
 * A gang block consists of a a gang header and up to gbh_nblkptrs(size)
 * gang members. The gang header is like an indirect block: it's an array
 * of block pointers, though the header has a small tail (a zio_eck_t)
 * that stores an embedded checksum. It is allocated using only a single
 * sector as the requested size, and hence is allocatable regardless of
 * fragmentation. Its size is determined by the smallest allocatable
 * asize of the vdevs it was allocated on. The gang header's bps point
 * to its gang members, which hold the data.
 *
 * Gang blocks are self-checksumming, using the bp's <vdev, offset, txg>
 * as the verifier to ensure uniqueness of the SHA256 checksum.
 * Critically, the gang block bp's blk_cksum is the checksum of the data,
 * not the gang header.  This ensures that data block signatures (needed for
 * deduplication) are independent of how the block is physically stored.
 *
 * Gang blocks can be nested: a gang member may itself be a gang block.
 * Thus every gang block is a tree in which root and all interior nodes are
 * gang headers, and the leaves are normal blocks that contain user data.
 * The root of the gang tree is called the gang leader.
 *
 * To perform any operation (read, rewrite, free, claim) on a gang block,
 * zio_gang_assemble() first assembles the gang tree (minus data leaves)
 * in the io_gang_tree field of the original logical i/o by recursively
 * reading the gang leader and all gang headers below it.  This yields
 * an in-core tree containing the contents of every gang header and the
 * bps for every constituent of the gang block.
 *
 * With the gang tree now assembled, zio_gang_issue() just walks the gang tree
 * and invokes a callback on each bp.  To free a gang block, zio_gang_issue()
 * calls zio_free_gang() -- a trivial wrapper around zio_free() -- for each bp.
 * zio_claim_gang() provides a similarly trivial wrapper for zio_claim().
 * zio_read_gang() is a wrapper around zio_read() that omits reading gang
 * headers, since we already have those in io_gang_tree.  zio_rewrite_gang()
 * performs a zio_rewrite() of the data or, for gang headers, a zio_rewrite()
 * of the gang header plus zio_checksum_compute() of the data to update the
 * gang header's blk_cksum as described above.
 *
 * The two-phase assemble/issue model solves the problem of partial failure --
 * what if you'd freed part of a gang block but then couldn't read the
 * gang header for another part?  Assembling the entire gang tree first
 * ensures that all the necessary gang header I/O has succeeded before
 * starting the actual work of free, claim, or write.  Once the gang tree
 * is assembled, free and claim are in-memory operations that cannot fail.
 *
 * In the event that a gang write fails, zio_dva_unallocate() walks the
 * gang tree to immediately free (i.e. insert back into the space map)
 * everything we've allocated.  This ensures that we don't get ENOSPC
 * errors during repeated suspend/resume cycles due to a flaky device.
 *
 * Gang rewrites only happen during sync-to-convergence.  If we can't assemble
 * the gang tree, we won't modify the block, so we can safely defer the free
 * (knowing that the block is still intact).  If we *can* assemble the gang
 * tree, then even if some of the rewrites fail, zio_dva_unallocate() will free
 * each constituent bp and we can allocate a new block on the next sync pass.
 *
 * In all cases, the gang tree allows complete recovery from partial failure.
 * ==========================================================================
 */

static void
zio_gang_issue_func_done(zio_t *zio)
{
	abd_free(zio->io_abd);
}

static zio_t *
zio_read_gang(zio_t *pio, blkptr_t *bp, zio_gang_node_t *gn, abd_t *data,
    uint64_t offset)
{
	if (gn != NULL)
		return (pio);

	return (zio_read(pio, pio->io_spa, bp, abd_get_offset(data, offset),
	    BP_GET_PSIZE(bp), zio_gang_issue_func_done,
	    NULL, pio->io_priority, ZIO_GANG_CHILD_FLAGS(pio),
	    &pio->io_bookmark));
}

static zio_t *
zio_rewrite_gang(zio_t *pio, blkptr_t *bp, zio_gang_node_t *gn, abd_t *data,
    uint64_t offset)
{
	zio_t *zio;

	if (gn != NULL) {
		abd_t *gbh_abd =
		    abd_get_from_buf(gn->gn_gbh, gn->gn_gangblocksize);
		zio = zio_rewrite(pio, pio->io_spa, pio->io_txg, bp,
		    gbh_abd, gn->gn_gangblocksize, zio_gang_issue_func_done,
		    NULL, pio->io_priority, ZIO_GANG_CHILD_FLAGS(pio),
		    &pio->io_bookmark);
		/*
		 * As we rewrite each gang header, the pipeline will compute
		 * a new gang block header checksum for it; but no one will
		 * compute a new data checksum, so we do that here.  The one
		 * exception is the gang leader: the pipeline already computed
		 * its data checksum because that stage precedes gang assembly.
		 * (Presently, nothing actually uses interior data checksums;
		 * this is just good hygiene.)
		 */
		if (gn != pio->io_gang_leader->io_gang_tree) {
			abd_t *buf = abd_get_offset(data, offset);

			zio_checksum_compute(zio, BP_GET_CHECKSUM(bp),
			    buf, BP_GET_PSIZE(bp));

			abd_free(buf);
		}
		/*
		 * If we are here to damage data for testing purposes,
		 * leave the GBH alone so that we can detect the damage.
		 */
		if (pio->io_gang_leader->io_flags & ZIO_FLAG_INDUCE_DAMAGE)
			zio->io_pipeline &= ~ZIO_VDEV_IO_STAGES;
	} else {
		zio = zio_rewrite(pio, pio->io_spa, pio->io_txg, bp,
		    abd_get_offset(data, offset), BP_GET_PSIZE(bp),
		    zio_gang_issue_func_done, NULL, pio->io_priority,
		    ZIO_GANG_CHILD_FLAGS(pio), &pio->io_bookmark);
	}

	return (zio);
}

static zio_t *
zio_free_gang(zio_t *pio, blkptr_t *bp, zio_gang_node_t *gn, abd_t *data,
    uint64_t offset)
{
	(void) gn, (void) data, (void) offset;

	zio_t *zio = zio_free_sync(pio, pio->io_spa, pio->io_txg, bp,
	    ZIO_GANG_CHILD_FLAGS(pio));
	if (zio == NULL) {
		zio = zio_null(pio, pio->io_spa,
		    NULL, NULL, NULL, ZIO_GANG_CHILD_FLAGS(pio));
	}
	return (zio);
}

static zio_t *
zio_claim_gang(zio_t *pio, blkptr_t *bp, zio_gang_node_t *gn, abd_t *data,
    uint64_t offset)
{
	(void) gn, (void) data, (void) offset;
	return (zio_claim(pio, pio->io_spa, pio->io_txg, bp,
	    NULL, NULL, ZIO_GANG_CHILD_FLAGS(pio)));
}

static zio_gang_issue_func_t *zio_gang_issue_func[ZIO_TYPES] = {
	NULL,
	zio_read_gang,
	zio_rewrite_gang,
	zio_free_gang,
	zio_claim_gang,
	NULL
};

static void zio_gang_tree_assemble_done(zio_t *zio);

static zio_gang_node_t *
zio_gang_node_alloc(zio_gang_node_t **gnpp, uint64_t gangblocksize)
{
	zio_gang_node_t *gn;

	ASSERT(*gnpp == NULL);

	gn = kmem_zalloc(sizeof (*gn) +
	    (gbh_nblkptrs(gangblocksize) * sizeof (gn)), KM_SLEEP);
	gn->gn_gangblocksize = gn->gn_allocsize = gangblocksize;
	gn->gn_gbh = zio_buf_alloc(gangblocksize);
	*gnpp = gn;

	return (gn);
}

static void
zio_gang_node_free(zio_gang_node_t **gnpp)
{
	zio_gang_node_t *gn = *gnpp;

	for (int g = 0; g < gbh_nblkptrs(gn->gn_allocsize); g++)
		ASSERT(gn->gn_child[g] == NULL);

	zio_buf_free(gn->gn_gbh, gn->gn_allocsize);
	kmem_free(gn, sizeof (*gn) +
	    (gbh_nblkptrs(gn->gn_allocsize) * sizeof (gn)));
	*gnpp = NULL;
}

static void
zio_gang_tree_free(zio_gang_node_t **gnpp)
{
	zio_gang_node_t *gn = *gnpp;

	if (gn == NULL)
		return;

	for (int g = 0; g < gbh_nblkptrs(gn->gn_allocsize); g++)
		zio_gang_tree_free(&gn->gn_child[g]);

	zio_gang_node_free(gnpp);
}

static void
zio_gang_tree_assemble(zio_t *gio, blkptr_t *bp, zio_gang_node_t **gnpp)
{
	uint64_t gangblocksize = UINT64_MAX;
	if (spa_feature_is_active(gio->io_spa,
	    SPA_FEATURE_DYNAMIC_GANG_HEADER)) {
		spa_config_enter(gio->io_spa, SCL_VDEV, FTAG, RW_READER);
		for (int dva = 0; dva < BP_GET_NDVAS(bp); dva++) {
			vdev_t *vd = vdev_lookup_top(gio->io_spa,
			    DVA_GET_VDEV(&bp->blk_dva[dva]));
			uint64_t asize = vdev_gang_header_asize(vd);
			gangblocksize = MIN(gangblocksize, asize);
		}
		spa_config_exit(gio->io_spa, SCL_VDEV, FTAG);
	} else {
		gangblocksize = SPA_OLD_GANGBLOCKSIZE;
	}
	ASSERT3U(gangblocksize, !=, UINT64_MAX);
	zio_gang_node_t *gn = zio_gang_node_alloc(gnpp, gangblocksize);
	abd_t *gbh_abd = abd_get_from_buf(gn->gn_gbh, gangblocksize);

	ASSERT(gio->io_gang_leader == gio);
	ASSERT(BP_IS_GANG(bp));

	zio_nowait(zio_read(gio, gio->io_spa, bp, gbh_abd, gangblocksize,
	    zio_gang_tree_assemble_done, gn, gio->io_priority,
	    ZIO_GANG_CHILD_FLAGS(gio), &gio->io_bookmark));
}

static void
zio_gang_tree_assemble_done(zio_t *zio)
{
	zio_t *gio = zio->io_gang_leader;
	zio_gang_node_t *gn = zio->io_private;
	blkptr_t *bp = zio->io_bp;

	ASSERT(gio == zio_unique_parent(zio));
	ASSERT(list_is_empty(&zio->io_child_list));

	if (zio->io_error)
		return;

	/* this ABD was created from a linear buf in zio_gang_tree_assemble */
	if (BP_SHOULD_BYTESWAP(bp))
		byteswap_uint64_array(abd_to_buf(zio->io_abd), zio->io_size);

	ASSERT3P(abd_to_buf(zio->io_abd), ==, gn->gn_gbh);
	/*
	 * If this was an old-style gangblock, the gangblocksize should have
	 * been updated in zio_checksum_error to reflect that.
	 */
	ASSERT3U(gbh_eck(gn->gn_gbh, gn->gn_gangblocksize)->zec_magic,
	    ==, ZEC_MAGIC);

	abd_free(zio->io_abd);

	for (int g = 0; g < gbh_nblkptrs(gn->gn_gangblocksize); g++) {
		blkptr_t *gbp = gbh_bp(gn->gn_gbh, g);
		if (!BP_IS_GANG(gbp))
			continue;
		zio_gang_tree_assemble(gio, gbp, &gn->gn_child[g]);
	}
}

static void
zio_gang_tree_issue(zio_t *pio, zio_gang_node_t *gn, blkptr_t *bp, abd_t *data,
    uint64_t offset)
{
	zio_t *gio = pio->io_gang_leader;
	zio_t *zio;

	ASSERT(BP_IS_GANG(bp) == !!gn);
	ASSERT(BP_GET_CHECKSUM(bp) == BP_GET_CHECKSUM(gio->io_bp));
	ASSERT(BP_GET_LSIZE(bp) == BP_GET_PSIZE(bp) || gn == gio->io_gang_tree);

	/*
	 * If you're a gang header, your data is in gn->gn_gbh.
	 * If you're a gang member, your data is in 'data' and gn == NULL.
	 */
	zio = zio_gang_issue_func[gio->io_type](pio, bp, gn, data, offset);

	if (gn != NULL) {
		ASSERT3U(gbh_eck(gn->gn_gbh,
		    gn->gn_gangblocksize)->zec_magic, ==, ZEC_MAGIC);

		for (int g = 0; g < gbh_nblkptrs(gn->gn_gangblocksize); g++) {
			blkptr_t *gbp = gbh_bp(gn->gn_gbh, g);
			if (BP_IS_HOLE(gbp))
				continue;
			zio_gang_tree_issue(zio, gn->gn_child[g], gbp, data,
			    offset);
			offset += BP_GET_PSIZE(gbp);
		}
	}

	if (gn == gio->io_gang_tree)
		ASSERT3U(gio->io_size, ==, offset);

	if (zio != pio)
		zio_nowait(zio);
}

static zio_t *
zio_gang_assemble(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;

	ASSERT(BP_IS_GANG(bp) && zio->io_gang_leader == NULL);
	ASSERT(zio->io_child_type > ZIO_CHILD_GANG);

	zio->io_gang_leader = zio;

	zio_gang_tree_assemble(zio, bp, &zio->io_gang_tree);

	return (zio);
}

static zio_t *
zio_gang_issue(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;

	if (zio_wait_for_children(zio, ZIO_CHILD_GANG_BIT, ZIO_WAIT_DONE)) {
		return (NULL);
	}

	ASSERT(BP_IS_GANG(bp) && zio->io_gang_leader == zio);
	ASSERT(zio->io_child_type > ZIO_CHILD_GANG);

	if (zio->io_child_error[ZIO_CHILD_GANG] == 0)
		zio_gang_tree_issue(zio, zio->io_gang_tree, bp, zio->io_abd,
		    0);
	else
		zio_gang_tree_free(&zio->io_gang_tree);

	zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;

	return (zio);
}

static void
zio_gang_inherit_allocator(zio_t *pio, zio_t *cio)
{
	cio->io_allocator = pio->io_allocator;
}

static void
zio_write_gang_member_ready(zio_t *zio)
{
	zio_t *pio = zio_unique_parent(zio);
	dva_t *cdva = zio->io_bp->blk_dva;
	dva_t *pdva = pio->io_bp->blk_dva;
	uint64_t asize;
	zio_t *gio __maybe_unused = zio->io_gang_leader;

	if (BP_IS_HOLE(zio->io_bp))
		return;

	/*
	 * If we're getting direct-invoked from zio_write_gang_block(),
	 * the bp_orig will be set.
	 */
	ASSERT(BP_IS_HOLE(&zio->io_bp_orig) ||
	    zio->io_flags & ZIO_FLAG_PREALLOCATED);

	ASSERT(zio->io_child_type == ZIO_CHILD_GANG);
	ASSERT3U(zio->io_prop.zp_copies, ==, gio->io_prop.zp_copies);
	ASSERT3U(zio->io_prop.zp_copies, <=, BP_GET_NDVAS(zio->io_bp));
	ASSERT3U(pio->io_prop.zp_copies, <=, BP_GET_NDVAS(pio->io_bp));
	VERIFY3U(BP_GET_NDVAS(zio->io_bp), <=, BP_GET_NDVAS(pio->io_bp));

	mutex_enter(&pio->io_lock);
	for (int d = 0; d < BP_GET_NDVAS(zio->io_bp); d++) {
		ASSERT(DVA_GET_GANG(&pdva[d]));
		asize = DVA_GET_ASIZE(&pdva[d]);
		asize += DVA_GET_ASIZE(&cdva[d]);
		DVA_SET_ASIZE(&pdva[d], asize);
	}
	mutex_exit(&pio->io_lock);
}

static void
zio_write_gang_done(zio_t *zio)
{
	/*
	 * The io_abd field will be NULL for a zio with no data.  The io_flags
	 * will initially have the ZIO_FLAG_NODATA bit flag set, but we can't
	 * check for it here as it is cleared in zio_ready.
	 */
	if (zio->io_abd != NULL)
		abd_free(zio->io_abd);
}

static void
zio_update_feature(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	spa_feature_incr(spa, (spa_feature_t)(uintptr_t)arg, tx);
}

static zio_t *
zio_write_gang_block(zio_t *pio, metaslab_class_t *mc)
{
	spa_t *spa = pio->io_spa;
	blkptr_t *bp = pio->io_bp;
	zio_t *gio = pio->io_gang_leader;
	zio_t *zio;
	zio_gang_node_t *gn, **gnpp;
	zio_gbh_phys_t *gbh;
	abd_t *gbh_abd;
	uint64_t txg = pio->io_txg;
	uint64_t resid = pio->io_size;
	zio_prop_t zp;
	int error;
	boolean_t has_data = !(pio->io_flags & ZIO_FLAG_NODATA);

	/*
	 * Store multiple copies of the GBH, so that we can still traverse
	 * all the data (e.g. to free or scrub) even if a block is damaged.
	 * This value respects the redundant_metadata property.
	 */
	int gbh_copies = gio->io_prop.zp_gang_copies;
	if (gbh_copies == 0) {
		/*
		 * This should only happen in the case where we're filling in
		 * DDT entries for a parent that wants more copies than the DDT
		 * has.  In that case, we cannot gang without creating a mixed
		 * blkptr, which is illegal.
		 */
		ASSERT3U(gio->io_child_type, ==, ZIO_CHILD_DDT);
		pio->io_error = EAGAIN;
		return (pio);
	}
	ASSERT3S(gbh_copies, >, 0);
	ASSERT3S(gbh_copies, <=, SPA_DVAS_PER_BP);

	ASSERT(ZIO_HAS_ALLOCATOR(pio));
	int flags = METASLAB_GANG_HEADER;
	if (pio->io_flags & ZIO_FLAG_ALLOC_THROTTLED) {
		ASSERT(pio->io_priority == ZIO_PRIORITY_ASYNC_WRITE);
		ASSERT(has_data);

		flags |= METASLAB_ASYNC_ALLOC;
	}

	uint64_t gangblocksize = SPA_OLD_GANGBLOCKSIZE;
	uint64_t candidate = gangblocksize;
	error = metaslab_alloc_range(spa, mc, gangblocksize, gangblocksize,
	    bp, gbh_copies, txg, pio == gio ? NULL : gio->io_bp, flags,
	    &pio->io_alloc_list, pio->io_allocator, pio, &candidate);
	if (error) {
		pio->io_error = error;
		return (pio);
	}
	if (spa_feature_is_active(spa, SPA_FEATURE_DYNAMIC_GANG_HEADER))
		gangblocksize = candidate;

	if (pio == gio) {
		gnpp = &gio->io_gang_tree;
	} else {
		gnpp = pio->io_private;
		ASSERT(pio->io_ready == zio_write_gang_member_ready);
	}

	gn = zio_gang_node_alloc(gnpp, gangblocksize);
	gbh = gn->gn_gbh;
	memset(gbh, 0, gangblocksize);
	gbh_abd = abd_get_from_buf(gbh, gangblocksize);

	/*
	 * Create the gang header.
	 */
	zio = zio_rewrite(pio, spa, txg, bp, gbh_abd, gangblocksize,
	    zio_write_gang_done, NULL, pio->io_priority,
	    ZIO_GANG_CHILD_FLAGS(pio), &pio->io_bookmark);

	zio_gang_inherit_allocator(pio, zio);
	if (pio->io_flags & ZIO_FLAG_ALLOC_THROTTLED) {
		boolean_t more;
		VERIFY(metaslab_class_throttle_reserve(mc, zio->io_allocator,
		    gbh_copies, zio->io_size, B_TRUE, &more));
		zio->io_flags |= ZIO_FLAG_ALLOC_THROTTLED;
	}

	/*
	 * Create and nowait the gang children. First, we try to do
	 * opportunistic allocations. If that fails to generate enough
	 * space, we fall back to normal zio_write calls for nested gang.
	 */
	int g;
	boolean_t any_failed = B_FALSE;
	for (g = 0; resid != 0; g++) {
		flags &= METASLAB_ASYNC_ALLOC;
		flags |= METASLAB_GANG_CHILD;
		zp.zp_checksum = gio->io_prop.zp_checksum;
		zp.zp_compress = ZIO_COMPRESS_OFF;
		zp.zp_complevel = gio->io_prop.zp_complevel;
		zp.zp_type = zp.zp_storage_type = DMU_OT_NONE;
		zp.zp_level = 0;
		zp.zp_copies = gio->io_prop.zp_copies;
		zp.zp_gang_copies = gio->io_prop.zp_gang_copies;
		zp.zp_dedup = B_FALSE;
		zp.zp_dedup_verify = B_FALSE;
		zp.zp_nopwrite = B_FALSE;
		zp.zp_encrypt = gio->io_prop.zp_encrypt;
		zp.zp_byteorder = gio->io_prop.zp_byteorder;
		zp.zp_direct_write = B_FALSE;
		memset(zp.zp_salt, 0, ZIO_DATA_SALT_LEN);
		memset(zp.zp_iv, 0, ZIO_DATA_IV_LEN);
		memset(zp.zp_mac, 0, ZIO_DATA_MAC_LEN);

		uint64_t min_size = zio_roundup_alloc_size(spa,
		    resid / (gbh_nblkptrs(gangblocksize) - g));
		min_size = MIN(min_size, resid);
		bp = &((blkptr_t *)gbh)[g];

		zio_alloc_list_t cio_list;
		metaslab_trace_init(&cio_list);
		uint64_t allocated_size = UINT64_MAX;
		error = metaslab_alloc_range(spa, mc, min_size, resid,
		    bp, gio->io_prop.zp_copies, txg, NULL,
		    flags, &cio_list, zio->io_allocator, NULL, &allocated_size);

		boolean_t allocated = error == 0;
		any_failed |= !allocated;

		uint64_t psize = allocated ? MIN(resid, allocated_size) :
		    min_size;
		ASSERT3U(psize, >=, min_size);

		zio_t *cio = zio_write(zio, spa, txg, bp, has_data ?
		    abd_get_offset(pio->io_abd, pio->io_size - resid) : NULL,
		    psize, psize, &zp, zio_write_gang_member_ready, NULL,
		    zio_write_gang_done, &gn->gn_child[g], pio->io_priority,
		    ZIO_GANG_CHILD_FLAGS(pio) |
		    (allocated ? ZIO_FLAG_PREALLOCATED : 0), &pio->io_bookmark);

		resid -= psize;
		zio_gang_inherit_allocator(zio, cio);
		if (allocated) {
			metaslab_trace_move(&cio_list, &cio->io_alloc_list);
			metaslab_group_alloc_increment_all(spa,
			    &cio->io_bp_orig, zio->io_allocator, flags, psize,
			    cio);
		}
		/*
		 * We do not reserve for the child writes, since we already
		 * reserved for the parent.  Unreserve though will be called
		 * for individual children.  We can do this since sum of all
		 * child's physical sizes is equal to parent's physical size.
		 * It would not work for potentially bigger allocation sizes.
		 */

		zio_nowait(cio);
	}

	/*
	 * If we used more gang children than the old limit, we must already be
	 * using the new headers. No need to update anything, just move on.
	 *
	 * Otherwise, we might be in a case where we need to turn on the new
	 * feature, so we check that. We enable the new feature if we didn't
	 * manage to fit everything into 3 gang children and we could have
	 * written more than that.
	 */
	if (g > gbh_nblkptrs(SPA_OLD_GANGBLOCKSIZE)) {
		ASSERT(spa_feature_is_active(spa,
		    SPA_FEATURE_DYNAMIC_GANG_HEADER));
	} else if (any_failed && candidate > SPA_OLD_GANGBLOCKSIZE &&
	    spa_feature_is_enabled(spa, SPA_FEATURE_DYNAMIC_GANG_HEADER) &&
	    !spa_feature_is_active(spa, SPA_FEATURE_DYNAMIC_GANG_HEADER)) {
		dmu_tx_t *tx =
		    dmu_tx_create_assigned(spa->spa_dsl_pool, txg + 1);
		dsl_sync_task_nowait(spa->spa_dsl_pool,
		    zio_update_feature,
		    (void *)SPA_FEATURE_DYNAMIC_GANG_HEADER, tx);
		dmu_tx_commit(tx);
	}

	/*
	 * Set pio's pipeline to just wait for zio to finish.
	 */
	pio->io_pipeline = ZIO_INTERLOCK_PIPELINE;

	zio_nowait(zio);

	return (pio);
}

/*
 * The zio_nop_write stage in the pipeline determines if allocating a
 * new bp is necessary.  The nopwrite feature can handle writes in
 * either syncing or open context (i.e. zil writes) and as a result is
 * mutually exclusive with dedup.
 *
 * By leveraging a cryptographically secure checksum, such as SHA256, we
 * can compare the checksums of the new data and the old to determine if
 * allocating a new block is required.  Note that our requirements for
 * cryptographic strength are fairly weak: there can't be any accidental
 * hash collisions, but we don't need to be secure against intentional
 * (malicious) collisions.  To trigger a nopwrite, you have to be able
 * to write the file to begin with, and triggering an incorrect (hash
 * collision) nopwrite is no worse than simply writing to the file.
 * That said, there are no known attacks against the checksum algorithms
 * used for nopwrite, assuming that the salt and the checksums
 * themselves remain secret.
 */
static zio_t *
zio_nop_write(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	blkptr_t *bp_orig = &zio->io_bp_orig;
	zio_prop_t *zp = &zio->io_prop;

	ASSERT(BP_IS_HOLE(bp));
	ASSERT(BP_GET_LEVEL(bp) == 0);
	ASSERT(!(zio->io_flags & ZIO_FLAG_IO_REWRITE));
	ASSERT(zp->zp_nopwrite);
	ASSERT(!zp->zp_dedup);
	ASSERT(zio->io_bp_override == NULL);
	ASSERT(IO_IS_ALLOCATING(zio));

	/*
	 * Check to see if the original bp and the new bp have matching
	 * characteristics (i.e. same checksum, compression algorithms, etc).
	 * If they don't then just continue with the pipeline which will
	 * allocate a new bp.
	 */
	if (BP_IS_HOLE(bp_orig) ||
	    !(zio_checksum_table[BP_GET_CHECKSUM(bp)].ci_flags &
	    ZCHECKSUM_FLAG_NOPWRITE) ||
	    BP_IS_ENCRYPTED(bp) || BP_IS_ENCRYPTED(bp_orig) ||
	    BP_GET_CHECKSUM(bp) != BP_GET_CHECKSUM(bp_orig) ||
	    BP_GET_COMPRESS(bp) != BP_GET_COMPRESS(bp_orig) ||
	    BP_GET_DEDUP(bp) != BP_GET_DEDUP(bp_orig) ||
	    zp->zp_copies != BP_GET_NDVAS(bp_orig))
		return (zio);

	/*
	 * If the checksums match then reset the pipeline so that we
	 * avoid allocating a new bp and issuing any I/O.
	 */
	if (ZIO_CHECKSUM_EQUAL(bp->blk_cksum, bp_orig->blk_cksum)) {
		ASSERT(zio_checksum_table[zp->zp_checksum].ci_flags &
		    ZCHECKSUM_FLAG_NOPWRITE);
		ASSERT3U(BP_GET_PSIZE(bp), ==, BP_GET_PSIZE(bp_orig));
		ASSERT3U(BP_GET_LSIZE(bp), ==, BP_GET_LSIZE(bp_orig));
		ASSERT(zp->zp_compress != ZIO_COMPRESS_OFF);
		ASSERT3U(bp->blk_prop, ==, bp_orig->blk_prop);

		/*
		 * If we're overwriting a block that is currently on an
		 * indirect vdev, then ignore the nopwrite request and
		 * allow a new block to be allocated on a concrete vdev.
		 */
		spa_config_enter(zio->io_spa, SCL_VDEV, FTAG, RW_READER);
		for (int d = 0; d < BP_GET_NDVAS(bp_orig); d++) {
			vdev_t *tvd = vdev_lookup_top(zio->io_spa,
			    DVA_GET_VDEV(&bp_orig->blk_dva[d]));
			if (tvd->vdev_ops == &vdev_indirect_ops) {
				spa_config_exit(zio->io_spa, SCL_VDEV, FTAG);
				return (zio);
			}
		}
		spa_config_exit(zio->io_spa, SCL_VDEV, FTAG);

		*bp = *bp_orig;
		zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;
		zio->io_flags |= ZIO_FLAG_NOPWRITE;
	}

	return (zio);
}

/*
 * ==========================================================================
 * Block Reference Table
 * ==========================================================================
 */
static zio_t *
zio_brt_free(zio_t *zio)
{
	blkptr_t *bp;

	bp = zio->io_bp;

	if (BP_GET_LEVEL(bp) > 0 ||
	    BP_IS_METADATA(bp) ||
	    !brt_maybe_exists(zio->io_spa, bp)) {
		return (zio);
	}

	if (!brt_entry_decref(zio->io_spa, bp)) {
		/*
		 * This isn't the last reference, so we cannot free
		 * the data yet.
		 */
		zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;
	}

	return (zio);
}

/*
 * ==========================================================================
 * Dedup
 * ==========================================================================
 */
static void
zio_ddt_child_read_done(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	ddt_t *ddt;
	ddt_entry_t *dde = zio->io_private;
	zio_t *pio = zio_unique_parent(zio);

	mutex_enter(&pio->io_lock);
	ddt = ddt_select(zio->io_spa, bp);

	if (zio->io_error == 0) {
		ddt_phys_variant_t v = ddt_phys_select(ddt, dde, bp);
		/* this phys variant doesn't need repair */
		ddt_phys_clear(dde->dde_phys, v);
	}

	if (zio->io_error == 0 && dde->dde_io->dde_repair_abd == NULL)
		dde->dde_io->dde_repair_abd = zio->io_abd;
	else
		abd_free(zio->io_abd);
	mutex_exit(&pio->io_lock);
}

static zio_t *
zio_ddt_read_start(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;

	ASSERT(BP_GET_DEDUP(bp));
	ASSERT(BP_GET_PSIZE(bp) == zio->io_size);
	ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);

	if (zio->io_child_error[ZIO_CHILD_DDT]) {
		ddt_t *ddt = ddt_select(zio->io_spa, bp);
		ddt_entry_t *dde = ddt_repair_start(ddt, bp);
		ddt_phys_variant_t v_self = ddt_phys_select(ddt, dde, bp);
		ddt_univ_phys_t *ddp = dde->dde_phys;
		blkptr_t blk;

		ASSERT(zio->io_vsd == NULL);
		zio->io_vsd = dde;

		if (v_self == DDT_PHYS_NONE)
			return (zio);

		/* issue I/O for the other copies */
		for (int p = 0; p < DDT_NPHYS(ddt); p++) {
			ddt_phys_variant_t v = DDT_PHYS_VARIANT(ddt, p);

			if (ddt_phys_birth(ddp, v) == 0 || v == v_self)
				continue;

			ddt_bp_create(ddt->ddt_checksum, &dde->dde_key,
			    ddp, v, &blk);
			zio_nowait(zio_read(zio, zio->io_spa, &blk,
			    abd_alloc_for_io(zio->io_size, B_TRUE),
			    zio->io_size, zio_ddt_child_read_done, dde,
			    zio->io_priority, ZIO_DDT_CHILD_FLAGS(zio) |
			    ZIO_FLAG_DONT_PROPAGATE, &zio->io_bookmark));
		}
		return (zio);
	}

	zio_nowait(zio_read(zio, zio->io_spa, bp,
	    zio->io_abd, zio->io_size, NULL, NULL, zio->io_priority,
	    ZIO_DDT_CHILD_FLAGS(zio), &zio->io_bookmark));

	return (zio);
}

static zio_t *
zio_ddt_read_done(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;

	if (zio_wait_for_children(zio, ZIO_CHILD_DDT_BIT, ZIO_WAIT_DONE)) {
		return (NULL);
	}

	ASSERT(BP_GET_DEDUP(bp));
	ASSERT(BP_GET_PSIZE(bp) == zio->io_size);
	ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);

	if (zio->io_child_error[ZIO_CHILD_DDT]) {
		ddt_t *ddt = ddt_select(zio->io_spa, bp);
		ddt_entry_t *dde = zio->io_vsd;
		if (ddt == NULL) {
			ASSERT(spa_load_state(zio->io_spa) != SPA_LOAD_NONE);
			return (zio);
		}
		if (dde == NULL) {
			zio->io_stage = ZIO_STAGE_DDT_READ_START >> 1;
			zio_taskq_dispatch(zio, ZIO_TASKQ_ISSUE, B_FALSE);
			return (NULL);
		}
		if (dde->dde_io->dde_repair_abd != NULL) {
			abd_copy(zio->io_abd, dde->dde_io->dde_repair_abd,
			    zio->io_size);
			zio->io_child_error[ZIO_CHILD_DDT] = 0;
		}
		ddt_repair_done(ddt, dde);
		zio->io_vsd = NULL;
	}

	ASSERT(zio->io_vsd == NULL);

	return (zio);
}

static boolean_t
zio_ddt_collision(zio_t *zio, ddt_t *ddt, ddt_entry_t *dde)
{
	spa_t *spa = zio->io_spa;
	boolean_t do_raw = !!(zio->io_flags & ZIO_FLAG_RAW);

	ASSERT(!(zio->io_bp_override && do_raw));

	/*
	 * Note: we compare the original data, not the transformed data,
	 * because when zio->io_bp is an override bp, we will not have
	 * pushed the I/O transforms.  That's an important optimization
	 * because otherwise we'd compress/encrypt all dmu_sync() data twice.
	 * However, we should never get a raw, override zio so in these
	 * cases we can compare the io_abd directly. This is useful because
	 * it allows us to do dedup verification even if we don't have access
	 * to the original data (for instance, if the encryption keys aren't
	 * loaded).
	 */

	for (int p = 0; p < DDT_NPHYS(ddt); p++) {
		if (DDT_PHYS_IS_DITTO(ddt, p))
			continue;

		if (dde->dde_io == NULL)
			continue;

		zio_t *lio = dde->dde_io->dde_lead_zio[p];
		if (lio == NULL)
			continue;

		if (do_raw)
			return (lio->io_size != zio->io_size ||
			    abd_cmp(zio->io_abd, lio->io_abd) != 0);

		return (lio->io_orig_size != zio->io_orig_size ||
		    abd_cmp(zio->io_orig_abd, lio->io_orig_abd) != 0);
	}

	for (int p = 0; p < DDT_NPHYS(ddt); p++) {
		ddt_phys_variant_t v = DDT_PHYS_VARIANT(ddt, p);
		uint64_t phys_birth = ddt_phys_birth(dde->dde_phys, v);

		if (phys_birth != 0 && do_raw) {
			blkptr_t blk = *zio->io_bp;
			uint64_t psize;
			abd_t *tmpabd;
			int error;

			ddt_bp_fill(dde->dde_phys, v, &blk, phys_birth);
			psize = BP_GET_PSIZE(&blk);

			if (psize != zio->io_size)
				return (B_TRUE);

			ddt_exit(ddt);

			tmpabd = abd_alloc_for_io(psize, B_TRUE);

			error = zio_wait(zio_read(NULL, spa, &blk, tmpabd,
			    psize, NULL, NULL, ZIO_PRIORITY_SYNC_READ,
			    ZIO_FLAG_CANFAIL | ZIO_FLAG_SPECULATIVE |
			    ZIO_FLAG_RAW, &zio->io_bookmark));

			if (error == 0) {
				if (abd_cmp(tmpabd, zio->io_abd) != 0)
					error = SET_ERROR(ENOENT);
			}

			abd_free(tmpabd);
			ddt_enter(ddt);
			return (error != 0);
		} else if (phys_birth != 0) {
			arc_buf_t *abuf = NULL;
			arc_flags_t aflags = ARC_FLAG_WAIT;
			blkptr_t blk = *zio->io_bp;
			int error;

			ddt_bp_fill(dde->dde_phys, v, &blk, phys_birth);

			if (BP_GET_LSIZE(&blk) != zio->io_orig_size)
				return (B_TRUE);

			ddt_exit(ddt);

			error = arc_read(NULL, spa, &blk,
			    arc_getbuf_func, &abuf, ZIO_PRIORITY_SYNC_READ,
			    ZIO_FLAG_CANFAIL | ZIO_FLAG_SPECULATIVE,
			    &aflags, &zio->io_bookmark);

			if (error == 0) {
				if (abd_cmp_buf(zio->io_orig_abd, abuf->b_data,
				    zio->io_orig_size) != 0)
					error = SET_ERROR(ENOENT);
				arc_buf_destroy(abuf, &abuf);
			}

			ddt_enter(ddt);
			return (error != 0);
		}
	}

	return (B_FALSE);
}

static void
zio_ddt_child_write_done(zio_t *zio)
{
	ddt_t *ddt = ddt_select(zio->io_spa, zio->io_bp);
	ddt_entry_t *dde = zio->io_private;

	zio_link_t *zl = NULL;
	ASSERT3P(zio_walk_parents(zio, &zl), !=, NULL);

	int p = DDT_PHYS_FOR_COPIES(ddt, zio->io_prop.zp_copies);
	ddt_phys_variant_t v = DDT_PHYS_VARIANT(ddt, p);
	ddt_univ_phys_t *ddp = dde->dde_phys;

	ddt_enter(ddt);

	/* we're the lead, so once we're done there's no one else outstanding */
	if (dde->dde_io->dde_lead_zio[p] == zio)
		dde->dde_io->dde_lead_zio[p] = NULL;

	ddt_univ_phys_t *orig = &dde->dde_io->dde_orig_phys;

	if (zio->io_error != 0) {
		/*
		 * The write failed, so we're about to abort the entire IO
		 * chain. We need to revert the entry back to what it was at
		 * the last time it was successfully extended.
		 */
		ddt_phys_unextend(ddp, orig, v);
		ddt_phys_clear(orig, v);

		ddt_exit(ddt);
		return;
	}

	/*
	 * Add references for all dedup writes that were waiting on the
	 * physical one, skipping any other physical writes that are waiting.
	 */
	zio_t *pio;
	zl = NULL;
	while ((pio = zio_walk_parents(zio, &zl)) != NULL) {
		if (!(pio->io_flags & ZIO_FLAG_DDT_CHILD))
			ddt_phys_addref(ddp, v);
	}

	/*
	 * We've successfully added new DVAs to the entry. Clear the saved
	 * state or, if there's still outstanding IO, remember it so we can
	 * revert to a known good state if that IO fails.
	 */
	if (dde->dde_io->dde_lead_zio[p] == NULL)
		ddt_phys_clear(orig, v);
	else
		ddt_phys_copy(orig, ddp, v);

	ddt_exit(ddt);
}

static void
zio_ddt_child_write_ready(zio_t *zio)
{
	ddt_t *ddt = ddt_select(zio->io_spa, zio->io_bp);
	ddt_entry_t *dde = zio->io_private;

	zio_link_t *zl = NULL;
	ASSERT3P(zio_walk_parents(zio, &zl), !=, NULL);

	int p = DDT_PHYS_FOR_COPIES(ddt, zio->io_prop.zp_copies);
	ddt_phys_variant_t v = DDT_PHYS_VARIANT(ddt, p);

	if (ddt_phys_is_gang(dde->dde_phys, v)) {
		for (int i = 0; i < BP_GET_NDVAS(zio->io_bp); i++) {
			dva_t *d = &zio->io_bp->blk_dva[i];
			metaslab_group_alloc_decrement(zio->io_spa,
			    DVA_GET_VDEV(d), zio->io_allocator,
			    METASLAB_ASYNC_ALLOC, zio->io_size, zio);
		}
		zio->io_error = EAGAIN;
	}

	if (zio->io_error != 0)
		return;

	ddt_enter(ddt);

	ddt_phys_extend(dde->dde_phys, v, zio->io_bp);

	zio_t *pio;
	zl = NULL;
	while ((pio = zio_walk_parents(zio, &zl)) != NULL) {
		if (!(pio->io_flags & ZIO_FLAG_DDT_CHILD))
			ddt_bp_fill(dde->dde_phys, v, pio->io_bp, zio->io_txg);
	}

	ddt_exit(ddt);
}

static zio_t *
zio_ddt_write(zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	blkptr_t *bp = zio->io_bp;
	uint64_t txg = zio->io_txg;
	zio_prop_t *zp = &zio->io_prop;
	ddt_t *ddt = ddt_select(spa, bp);
	ddt_entry_t *dde;

	ASSERT(BP_GET_DEDUP(bp));
	ASSERT(BP_GET_CHECKSUM(bp) == zp->zp_checksum);
	ASSERT(BP_IS_HOLE(bp) || zio->io_bp_override);
	ASSERT(!(zio->io_bp_override && (zio->io_flags & ZIO_FLAG_RAW)));
	/*
	 * Deduplication will not take place for Direct I/O writes. The
	 * ddt_tree will be emptied in syncing context. Direct I/O writes take
	 * place in the open-context. Direct I/O write can not attempt to
	 * modify the ddt_tree while issuing out a write.
	 */
	ASSERT3B(zio->io_prop.zp_direct_write, ==, B_FALSE);

	ddt_enter(ddt);
	/*
	 * Search DDT for matching entry.  Skip DVAs verification here, since
	 * they can go only from override, and once we get here the override
	 * pointer can't have "D" flag to be confused with pruned DDT entries.
	 */
	IMPLY(zio->io_bp_override, !BP_GET_DEDUP(zio->io_bp_override));
	dde = ddt_lookup(ddt, bp, B_FALSE);
	if (dde == NULL) {
		/* DDT size is over its quota so no new entries */
		zp->zp_dedup = B_FALSE;
		BP_SET_DEDUP(bp, B_FALSE);
		if (zio->io_bp_override == NULL)
			zio->io_pipeline = ZIO_WRITE_PIPELINE;
		ddt_exit(ddt);
		return (zio);
	}

	if (zp->zp_dedup_verify && zio_ddt_collision(zio, ddt, dde)) {
		/*
		 * If we're using a weak checksum, upgrade to a strong checksum
		 * and try again.  If we're already using a strong checksum,
		 * we can't resolve it, so just convert to an ordinary write.
		 * (And automatically e-mail a paper to Nature?)
		 */
		if (!(zio_checksum_table[zp->zp_checksum].ci_flags &
		    ZCHECKSUM_FLAG_DEDUP)) {
			zp->zp_checksum = spa_dedup_checksum(spa);
			zio_pop_transforms(zio);
			zio->io_stage = ZIO_STAGE_OPEN;
			BP_ZERO(bp);
		} else {
			zp->zp_dedup = B_FALSE;
			BP_SET_DEDUP(bp, B_FALSE);
		}
		ASSERT(!BP_GET_DEDUP(bp));
		zio->io_pipeline = ZIO_WRITE_PIPELINE;
		ddt_exit(ddt);
		return (zio);
	}

	int p = DDT_PHYS_FOR_COPIES(ddt, zp->zp_copies);
	ddt_phys_variant_t v = DDT_PHYS_VARIANT(ddt, p);
	ddt_univ_phys_t *ddp = dde->dde_phys;

	/*
	 * In the common cases, at this point we have a regular BP with no
	 * allocated DVAs, and the corresponding DDT entry for its checksum.
	 * Our goal is to fill the BP with enough DVAs to satisfy its copies=
	 * requirement.
	 *
	 * One of three things needs to happen to fulfill this:
	 *
	 * - if the DDT entry has enough DVAs to satisfy the BP, we just copy
	 *   them out of the entry and return;
	 *
	 * - if the DDT entry has no DVAs (ie its brand new), then we have to
	 *   issue the write as normal so that DVAs can be allocated and the
	 *   data land on disk. We then copy the DVAs into the DDT entry on
	 *   return.
	 *
	 * - if the DDT entry has some DVAs, but too few, we have to issue the
	 *   write, adjusted to have allocate fewer copies. When it returns, we
	 *   add the new DVAs to the DDT entry, and update the BP to have the
	 *   full amount it originally requested.
	 *
	 * In all cases, if there's already a writing IO in flight, we need to
	 * defer the action until after the write is done. If our action is to
	 * write, we need to adjust our request for additional DVAs to match
	 * what will be in the DDT entry after it completes. In this way every
	 * IO can be guaranteed to recieve enough DVAs simply by joining the
	 * end of the chain and letting the sequence play out.
	 */

	/*
	 * Number of DVAs in the DDT entry. If the BP is encrypted we ignore
	 * the third one as normal.
	 */
	int have_dvas = ddt_phys_dva_count(ddp, v, BP_IS_ENCRYPTED(bp));
	IMPLY(have_dvas == 0, ddt_phys_birth(ddp, v) == 0);
	boolean_t is_ganged = ddt_phys_is_gang(ddp, v);

	/* Number of DVAs requested by the IO. */
	uint8_t need_dvas = zp->zp_copies;
	/* Number of DVAs in outstanding writes for this dde. */
	uint8_t parent_dvas = 0;

	/*
	 * What we do next depends on whether or not there's IO outstanding that
	 * will update this entry.
	 */
	if (dde->dde_io == NULL || dde->dde_io->dde_lead_zio[p] == NULL) {
		/*
		 * No IO outstanding, so we only need to worry about ourselves.
		 */

		/*
		 * Override BPs bring their own DVAs and their own problems.
		 */
		if (zio->io_bp_override) {
			/*
			 * For a brand-new entry, all the work has been done
			 * for us, and we can just fill it out from the provided
			 * block and leave.
			 */
			if (have_dvas == 0) {
				ASSERT(BP_GET_LOGICAL_BIRTH(bp) == txg);
				ASSERT(BP_EQUAL(bp, zio->io_bp_override));
				ddt_phys_extend(ddp, v, bp);
				ddt_phys_addref(ddp, v);
				ddt_exit(ddt);
				return (zio);
			}

			/*
			 * If we already have this entry, then we want to treat
			 * it like a regular write. To do this we just wipe
			 * them out and proceed like a regular write.
			 *
			 * Even if there are some DVAs in the entry, we still
			 * have to clear them out. We can't use them to fill
			 * out the dedup entry, as they are all referenced
			 * together by a bp already on disk, and will be freed
			 * as a group.
			 */
			BP_ZERO_DVAS(bp);
			BP_SET_BIRTH(bp, 0, 0);
		}

		/*
		 * If there are enough DVAs in the entry to service our request,
		 * then we can just use them as-is.
		 */
		if (have_dvas >= need_dvas) {
			ddt_bp_fill(ddp, v, bp, txg);
			ddt_phys_addref(ddp, v);
			ddt_exit(ddt);
			return (zio);
		}

		/*
		 * Otherwise, we have to issue IO to fill the entry up to the
		 * amount we need.
		 */
		need_dvas -= have_dvas;
	} else {
		/*
		 * There's a write in-flight. If there's already enough DVAs on
		 * the entry, then either there were already enough to start
		 * with, or the in-flight IO is between READY and DONE, and so
		 * has extended the entry with new DVAs. Either way, we don't
		 * need to do anything, we can just slot in behind it.
		 */

		if (zio->io_bp_override) {
			/*
			 * If there's a write out, then we're soon going to
			 * have our own copies of this block, so clear out the
			 * override block and treat it as a regular dedup
			 * write. See comment above.
			 */
			BP_ZERO_DVAS(bp);
			BP_SET_BIRTH(bp, 0, 0);
		}

		if (have_dvas >= need_dvas) {
			/*
			 * A minor point: there might already be enough
			 * committed DVAs in the entry to service our request,
			 * but we don't know which are completed and which are
			 * allocated but not yet written. In this case, should
			 * the IO for the new DVAs fail, we will be on the end
			 * of the IO chain and will also recieve an error, even
			 * though our request could have been serviced.
			 *
			 * This is an extremely rare case, as it requires the
			 * original block to be copied with a request for a
			 * larger number of DVAs, then copied again requesting
			 * the same (or already fulfilled) number of DVAs while
			 * the first request is active, and then that first
			 * request errors. In return, the logic required to
			 * catch and handle it is complex. For now, I'm just
			 * not going to bother with it.
			 */

			/*
			 * We always fill the bp here as we may have arrived
			 * after the in-flight write has passed READY, and so
			 * missed out.
			 */
			ddt_bp_fill(ddp, v, bp, txg);
			zio_add_child(zio, dde->dde_io->dde_lead_zio[p]);
			ddt_exit(ddt);
			return (zio);
		}

		/*
		 * There's not enough in the entry yet, so we need to look at
		 * the write in-flight and see how many DVAs it will have once
		 * it completes.
		 *
		 * The in-flight write has potentially had its copies request
		 * reduced (if we're filling out an existing entry), so we need
		 * to reach in and get the original write to find out what it is
		 * expecting.
		 *
		 * Note that the parent of the lead zio will always have the
		 * highest zp_copies of any zio in the chain, because ones that
		 * can be serviced without additional IO are always added to
		 * the back of the chain.
		 */
		zio_link_t *zl = NULL;
		zio_t *pio =
		    zio_walk_parents(dde->dde_io->dde_lead_zio[p], &zl);
		ASSERT(pio);
		parent_dvas = pio->io_prop.zp_copies;

		if (parent_dvas >= need_dvas) {
			zio_add_child(zio, dde->dde_io->dde_lead_zio[p]);
			ddt_exit(ddt);
			return (zio);
		}

		/*
		 * Still not enough, so we will need to issue to get the
		 * shortfall.
		 */
		need_dvas -= parent_dvas;
	}

	if (is_ganged) {
		zp->zp_dedup = B_FALSE;
		BP_SET_DEDUP(bp, B_FALSE);
		zio->io_pipeline = ZIO_WRITE_PIPELINE;
		ddt_exit(ddt);
		return (zio);
	}

	/*
	 * We need to write. We will create a new write with the copies
	 * property adjusted to match the number of DVAs we need to need to
	 * grow the DDT entry by to satisfy the request.
	 */
	zio_prop_t czp = *zp;
	if (have_dvas > 0 || parent_dvas > 0) {
		czp.zp_copies = need_dvas;
		czp.zp_gang_copies = 0;
	} else {
		ASSERT3U(czp.zp_copies, ==, need_dvas);
	}

	zio_t *cio = zio_write(zio, spa, txg, bp, zio->io_orig_abd,
	    zio->io_orig_size, zio->io_orig_size, &czp,
	    zio_ddt_child_write_ready, NULL,
	    zio_ddt_child_write_done, dde, zio->io_priority,
	    ZIO_DDT_CHILD_FLAGS(zio), &zio->io_bookmark);

	zio_push_transform(cio, zio->io_abd, zio->io_size, 0, NULL);

	/*
	 * We are the new lead zio, because our parent has the highest
	 * zp_copies that has been requested for this entry so far.
	 */
	ddt_alloc_entry_io(dde);
	if (dde->dde_io->dde_lead_zio[p] == NULL) {
		/*
		 * First time out, take a copy of the stable entry to revert
		 * to if there's an error (see zio_ddt_child_write_done())
		 */
		ddt_phys_copy(&dde->dde_io->dde_orig_phys, dde->dde_phys, v);
	} else {
		/*
		 * Make the existing chain our child, because it cannot
		 * complete until we have.
		 */
		zio_add_child(cio, dde->dde_io->dde_lead_zio[p]);
	}
	dde->dde_io->dde_lead_zio[p] = cio;

	ddt_exit(ddt);

	zio_nowait(cio);

	return (zio);
}

static ddt_entry_t *freedde; /* for debugging */

static zio_t *
zio_ddt_free(zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	blkptr_t *bp = zio->io_bp;
	ddt_t *ddt = ddt_select(spa, bp);
	ddt_entry_t *dde = NULL;

	ASSERT(BP_GET_DEDUP(bp));
	ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);

	ddt_enter(ddt);
	freedde = dde = ddt_lookup(ddt, bp, B_TRUE);
	if (dde) {
		ddt_phys_variant_t v = ddt_phys_select(ddt, dde, bp);
		if (v != DDT_PHYS_NONE)
			ddt_phys_decref(dde->dde_phys, v);
	}
	ddt_exit(ddt);

	/*
	 * When no entry was found, it must have been pruned,
	 * so we can free it now instead of decrementing the
	 * refcount in the DDT.
	 */
	if (!dde) {
		BP_SET_DEDUP(bp, 0);
		zio->io_pipeline |= ZIO_STAGE_DVA_FREE;
	}

	return (zio);
}

/*
 * ==========================================================================
 * Allocate and free blocks
 * ==========================================================================
 */

static zio_t *
zio_io_to_allocate(metaslab_class_allocator_t *mca, boolean_t *more)
{
	zio_t *zio;

	ASSERT(MUTEX_HELD(&mca->mca_lock));

	zio = avl_first(&mca->mca_tree);
	if (zio == NULL) {
		*more = B_FALSE;
		return (NULL);
	}

	ASSERT(IO_IS_ALLOCATING(zio));
	ASSERT(ZIO_HAS_ALLOCATOR(zio));

	/*
	 * Try to place a reservation for this zio. If we're unable to
	 * reserve then we throttle.
	 */
	if (!metaslab_class_throttle_reserve(zio->io_metaslab_class,
	    zio->io_allocator, zio->io_prop.zp_copies, zio->io_size,
	    B_FALSE, more)) {
		return (NULL);
	}
	zio->io_flags |= ZIO_FLAG_ALLOC_THROTTLED;

	avl_remove(&mca->mca_tree, zio);
	ASSERT3U(zio->io_stage, <, ZIO_STAGE_DVA_ALLOCATE);

	if (avl_is_empty(&mca->mca_tree))
		*more = B_FALSE;
	return (zio);
}

static zio_t *
zio_dva_throttle(zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	zio_t *nio;
	metaslab_class_t *mc;
	boolean_t more;

	/*
	 * If not already chosen, choose an appropriate allocation class.
	 */
	mc = zio->io_metaslab_class;
	if (mc == NULL)
		mc = spa_preferred_class(spa, zio);

	if (zio->io_priority == ZIO_PRIORITY_SYNC_WRITE ||
	    !mc->mc_alloc_throttle_enabled ||
	    zio->io_child_type == ZIO_CHILD_GANG ||
	    zio->io_flags & ZIO_FLAG_NODATA) {
		return (zio);
	}

	ASSERT(zio->io_type == ZIO_TYPE_WRITE);
	ASSERT(ZIO_HAS_ALLOCATOR(zio));
	ASSERT(zio->io_child_type > ZIO_CHILD_GANG);
	ASSERT3U(zio->io_queued_timestamp, >, 0);
	ASSERT(zio->io_stage == ZIO_STAGE_DVA_THROTTLE);

	zio->io_metaslab_class = mc;
	metaslab_class_allocator_t *mca = &mc->mc_allocator[zio->io_allocator];
	mutex_enter(&mca->mca_lock);
	avl_add(&mca->mca_tree, zio);
	nio = zio_io_to_allocate(mca, &more);
	mutex_exit(&mca->mca_lock);
	return (nio);
}

static void
zio_allocate_dispatch(metaslab_class_t *mc, int allocator)
{
	metaslab_class_allocator_t *mca = &mc->mc_allocator[allocator];
	zio_t *zio;
	boolean_t more;

	do {
		mutex_enter(&mca->mca_lock);
		zio = zio_io_to_allocate(mca, &more);
		mutex_exit(&mca->mca_lock);
		if (zio == NULL)
			return;

		ASSERT3U(zio->io_stage, ==, ZIO_STAGE_DVA_THROTTLE);
		ASSERT0(zio->io_error);
		zio_taskq_dispatch(zio, ZIO_TASKQ_ISSUE, B_TRUE);
	} while (more);
}

static zio_t *
zio_dva_allocate(zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	metaslab_class_t *mc, *newmc;
	blkptr_t *bp = zio->io_bp;
	int error;
	int flags = 0;

	if (zio->io_gang_leader == NULL) {
		ASSERT(zio->io_child_type > ZIO_CHILD_GANG);
		zio->io_gang_leader = zio;
	}
	if (zio->io_flags & ZIO_FLAG_PREALLOCATED) {
		ASSERT3U(zio->io_child_type, ==, ZIO_CHILD_GANG);
		memcpy(zio->io_bp->blk_dva, zio->io_bp_orig.blk_dva,
		    3 * sizeof (dva_t));
		BP_SET_BIRTH(zio->io_bp, BP_GET_LOGICAL_BIRTH(&zio->io_bp_orig),
		    BP_GET_PHYSICAL_BIRTH(&zio->io_bp_orig));
		return (zio);
	}

	ASSERT(BP_IS_HOLE(bp));
	ASSERT0(BP_GET_NDVAS(bp));
	ASSERT3U(zio->io_prop.zp_copies, >, 0);

	ASSERT3U(zio->io_prop.zp_copies, <=, spa_max_replication(spa));
	ASSERT3U(zio->io_size, ==, BP_GET_PSIZE(bp));

	if (zio->io_flags & ZIO_FLAG_GANG_CHILD)
		flags |= METASLAB_GANG_CHILD;
	if (zio->io_priority == ZIO_PRIORITY_ASYNC_WRITE)
		flags |= METASLAB_ASYNC_ALLOC;

	/*
	 * If not already chosen, choose an appropriate allocation class.
	 */
	mc = zio->io_metaslab_class;
	if (mc == NULL) {
		mc = spa_preferred_class(spa, zio);
		zio->io_metaslab_class = mc;
	}
	ZIOSTAT_BUMP(ziostat_total_allocations);

again:
	/*
	 * Try allocating the block in the usual metaslab class.
	 * If that's full, allocate it in some other class(es).
	 * If that's full, allocate as a gang block,
	 * and if all are full, the allocation fails (which shouldn't happen).
	 *
	 * Note that we do not fall back on embedded slog (ZIL) space, to
	 * preserve unfragmented slog space, which is critical for decent
	 * sync write performance.  If a log allocation fails, we will fall
	 * back to spa_sync() which is abysmal for performance.
	 */
	ASSERT(ZIO_HAS_ALLOCATOR(zio));
	error = metaslab_alloc(spa, mc, zio->io_size, bp,
	    zio->io_prop.zp_copies, zio->io_txg, NULL, flags,
	    &zio->io_alloc_list, zio->io_allocator, zio);

	/*
	 * When the dedup or special class is spilling into the normal class,
	 * there can still be significant space available due to deferred
	 * frees that are in-flight.  We track the txg when this occurred and
	 * back off adding new DDT entries for a few txgs to allow the free
	 * blocks to be processed.
	 */
	if (error == ENOSPC && spa->spa_dedup_class_full_txg != zio->io_txg &&
	    (mc == spa_dedup_class(spa) || (mc == spa_special_class(spa) &&
	    !spa_has_dedup(spa) && spa_special_has_ddt(spa)))) {
		spa->spa_dedup_class_full_txg = zio->io_txg;
		zfs_dbgmsg("%s[%llu]: %s class spilling, req size %llu, "
		    "%llu allocated of %llu",
		    spa_name(spa), (u_longlong_t)zio->io_txg,
		    metaslab_class_get_name(mc),
		    (u_longlong_t)zio->io_size,
		    (u_longlong_t)metaslab_class_get_alloc(mc),
		    (u_longlong_t)metaslab_class_get_space(mc));
	}

	/*
	 * Fall back to some other class when this one is full.
	 */
	if (error == ENOSPC && (newmc = spa_preferred_class(spa, zio)) != mc) {
		/*
		 * If we are holding old class reservation, drop it.
		 * Dispatch the next ZIO(s) there if some are waiting.
		 */
		if (zio->io_flags & ZIO_FLAG_ALLOC_THROTTLED) {
			if (metaslab_class_throttle_unreserve(mc,
			    zio->io_allocator, zio->io_prop.zp_copies,
			    zio->io_size)) {
				zio_allocate_dispatch(zio->io_metaslab_class,
				    zio->io_allocator);
			}
			zio->io_flags &= ~ZIO_FLAG_ALLOC_THROTTLED;
		}

		if (zfs_flags & ZFS_DEBUG_METASLAB_ALLOC) {
			zfs_dbgmsg("%s: metaslab allocation failure in %s "
			    "class, trying fallback to %s class: zio %px, "
			    "size %llu, error %d", spa_name(spa),
			    metaslab_class_get_name(mc),
			    metaslab_class_get_name(newmc),
			    zio, (u_longlong_t)zio->io_size, error);
		}
		zio->io_metaslab_class = mc = newmc;
		ZIOSTAT_BUMP(ziostat_alloc_class_fallbacks);

		/*
		 * If the new class uses throttling, return to that pipeline
		 * stage.  Otherwise just do another allocation attempt.
		 */
		if (zio->io_priority != ZIO_PRIORITY_SYNC_WRITE &&
		    mc->mc_alloc_throttle_enabled &&
		    zio->io_child_type != ZIO_CHILD_GANG &&
		    !(zio->io_flags & ZIO_FLAG_NODATA)) {
			zio->io_stage = ZIO_STAGE_DVA_THROTTLE >> 1;
			return (zio);
		}
		goto again;
	}

	if (error == ENOSPC && zio->io_size > spa->spa_min_alloc) {
		if (zfs_flags & ZFS_DEBUG_METASLAB_ALLOC) {
			zfs_dbgmsg("%s: metaslab allocation failure, "
			    "trying ganging: zio %px, size %llu, error %d",
			    spa_name(spa), zio, (u_longlong_t)zio->io_size,
			    error);
		}
		ZIOSTAT_BUMP(ziostat_gang_writes);
		if (flags & METASLAB_GANG_CHILD)
			ZIOSTAT_BUMP(ziostat_gang_multilevel);
		return (zio_write_gang_block(zio, mc));
	}
	if (error != 0) {
		if (error != ENOSPC ||
		    (zfs_flags & ZFS_DEBUG_METASLAB_ALLOC)) {
			zfs_dbgmsg("%s: metaslab allocation failure: zio %px, "
			    "size %llu, error %d",
			    spa_name(spa), zio, (u_longlong_t)zio->io_size,
			    error);
		}
		zio->io_error = error;
	}

	return (zio);
}

static zio_t *
zio_dva_free(zio_t *zio)
{
	metaslab_free(zio->io_spa, zio->io_bp, zio->io_txg, B_FALSE);

	return (zio);
}

static zio_t *
zio_dva_claim(zio_t *zio)
{
	int error;

	error = metaslab_claim(zio->io_spa, zio->io_bp, zio->io_txg);
	if (error)
		zio->io_error = error;

	return (zio);
}

/*
 * Undo an allocation.  This is used by zio_done() when an I/O fails
 * and we want to give back the block we just allocated.
 * This handles both normal blocks and gang blocks.
 */
static void
zio_dva_unallocate(zio_t *zio, zio_gang_node_t *gn, blkptr_t *bp)
{
	ASSERT(BP_GET_LOGICAL_BIRTH(bp) == zio->io_txg || BP_IS_HOLE(bp));
	ASSERT(zio->io_bp_override == NULL);

	if (!BP_IS_HOLE(bp)) {
		metaslab_free(zio->io_spa, bp, BP_GET_LOGICAL_BIRTH(bp),
		    B_TRUE);
	}

	if (gn != NULL) {
		for (int g = 0; g < gbh_nblkptrs(gn->gn_gangblocksize); g++) {
			zio_dva_unallocate(zio, gn->gn_child[g],
			    gbh_bp(gn->gn_gbh, g));
		}
	}
}

/*
 * Try to allocate an intent log block.  Return 0 on success, errno on failure.
 */
int
zio_alloc_zil(spa_t *spa, objset_t *os, uint64_t txg, blkptr_t *new_bp,
    uint64_t size, boolean_t *slog)
{
	int error = 1;
	zio_alloc_list_t io_alloc_list;

	ASSERT(txg > spa_syncing_txg(spa));

	metaslab_trace_init(&io_alloc_list);

	/*
	 * Block pointer fields are useful to metaslabs for stats and debugging.
	 * Fill in the obvious ones before calling into metaslab_alloc().
	 */
	BP_SET_TYPE(new_bp, DMU_OT_INTENT_LOG);
	BP_SET_PSIZE(new_bp, size);
	BP_SET_LEVEL(new_bp, 0);

	/*
	 * When allocating a zil block, we don't have information about
	 * the final destination of the block except the objset it's part
	 * of, so we just hash the objset ID to pick the allocator to get
	 * some parallelism.
	 */
	int flags = METASLAB_ZIL;
	int allocator = (uint_t)cityhash1(os->os_dsl_dataset->ds_object)
	    % spa->spa_alloc_count;
	ZIOSTAT_BUMP(ziostat_total_allocations);
	error = metaslab_alloc(spa, spa_log_class(spa), size, new_bp, 1,
	    txg, NULL, flags, &io_alloc_list, allocator, NULL);
	*slog = (error == 0);
	if (error != 0) {
		error = metaslab_alloc(spa, spa_embedded_log_class(spa), size,
		    new_bp, 1, txg, NULL, flags, &io_alloc_list, allocator,
		    NULL);
	}
	if (error != 0) {
		ZIOSTAT_BUMP(ziostat_alloc_class_fallbacks);
		error = metaslab_alloc(spa, spa_normal_class(spa), size,
		    new_bp, 1, txg, NULL, flags, &io_alloc_list, allocator,
		    NULL);
	}
	metaslab_trace_fini(&io_alloc_list);

	if (error == 0) {
		BP_SET_LSIZE(new_bp, size);
		BP_SET_PSIZE(new_bp, size);
		BP_SET_COMPRESS(new_bp, ZIO_COMPRESS_OFF);
		BP_SET_CHECKSUM(new_bp,
		    spa_version(spa) >= SPA_VERSION_SLIM_ZIL
		    ? ZIO_CHECKSUM_ZILOG2 : ZIO_CHECKSUM_ZILOG);
		BP_SET_TYPE(new_bp, DMU_OT_INTENT_LOG);
		BP_SET_LEVEL(new_bp, 0);
		BP_SET_DEDUP(new_bp, 0);
		BP_SET_BYTEORDER(new_bp, ZFS_HOST_BYTEORDER);

		/*
		 * encrypted blocks will require an IV and salt. We generate
		 * these now since we will not be rewriting the bp at
		 * rewrite time.
		 */
		if (os->os_encrypted) {
			uint8_t iv[ZIO_DATA_IV_LEN];
			uint8_t salt[ZIO_DATA_SALT_LEN];

			BP_SET_CRYPT(new_bp, B_TRUE);
			VERIFY0(spa_crypt_get_salt(spa,
			    dmu_objset_id(os), salt));
			VERIFY0(zio_crypt_generate_iv(iv));

			zio_crypt_encode_params_bp(new_bp, salt, iv);
		}
	} else {
		zfs_dbgmsg("%s: zil block allocation failure: "
		    "size %llu, error %d", spa_name(spa), (u_longlong_t)size,
		    error);
	}

	return (error);
}

/*
 * ==========================================================================
 * Read and write to physical devices
 * ==========================================================================
 */

/*
 * Issue an I/O to the underlying vdev. Typically the issue pipeline
 * stops after this stage and will resume upon I/O completion.
 * However, there are instances where the vdev layer may need to
 * continue the pipeline when an I/O was not issued. Since the I/O
 * that was sent to the vdev layer might be different than the one
 * currently active in the pipeline (see vdev_queue_io()), we explicitly
 * force the underlying vdev layers to call either zio_execute() or
 * zio_interrupt() to ensure that the pipeline continues with the correct I/O.
 */
static zio_t *
zio_vdev_io_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	uint64_t align;
	spa_t *spa = zio->io_spa;

	zio->io_delay = 0;

	ASSERT(zio->io_error == 0);
	ASSERT(zio->io_child_error[ZIO_CHILD_VDEV] == 0);

	if (vd == NULL) {
		if (!(zio->io_flags & ZIO_FLAG_CONFIG_WRITER))
			spa_config_enter(spa, SCL_ZIO, zio, RW_READER);

		/*
		 * The mirror_ops handle multiple DVAs in a single BP.
		 */
		vdev_mirror_ops.vdev_op_io_start(zio);
		return (NULL);
	}

	ASSERT3P(zio->io_logical, !=, zio);
	if (zio->io_type == ZIO_TYPE_WRITE) {
		ASSERT(spa->spa_trust_config);

		/*
		 * Note: the code can handle other kinds of writes,
		 * but we don't expect them.
		 */
		if (zio->io_vd->vdev_noalloc) {
			ASSERT(zio->io_flags &
			    (ZIO_FLAG_PHYSICAL | ZIO_FLAG_SELF_HEAL |
			    ZIO_FLAG_RESILVER | ZIO_FLAG_INDUCE_DAMAGE));
		}
	}

	align = 1ULL << vd->vdev_top->vdev_ashift;

	if (!(zio->io_flags & ZIO_FLAG_PHYSICAL) &&
	    P2PHASE(zio->io_size, align) != 0) {
		/* Transform logical writes to be a full physical block size. */
		uint64_t asize = P2ROUNDUP(zio->io_size, align);
		abd_t *abuf = abd_alloc_sametype(zio->io_abd, asize);
		ASSERT(vd == vd->vdev_top);
		if (zio->io_type == ZIO_TYPE_WRITE) {
			abd_copy(abuf, zio->io_abd, zio->io_size);
			abd_zero_off(abuf, zio->io_size, asize - zio->io_size);
		}
		zio_push_transform(zio, abuf, asize, asize, zio_subblock);
	}

	/*
	 * If this is not a physical io, make sure that it is properly aligned
	 * before proceeding.
	 */
	if (!(zio->io_flags & ZIO_FLAG_PHYSICAL)) {
		ASSERT0(P2PHASE(zio->io_offset, align));
		ASSERT0(P2PHASE(zio->io_size, align));
	} else {
		/*
		 * For physical writes, we allow 512b aligned writes and assume
		 * the device will perform a read-modify-write as necessary.
		 */
		ASSERT0(P2PHASE(zio->io_offset, SPA_MINBLOCKSIZE));
		ASSERT0(P2PHASE(zio->io_size, SPA_MINBLOCKSIZE));
	}

	VERIFY(zio->io_type != ZIO_TYPE_WRITE || spa_writeable(spa));

	/*
	 * If this is a repair I/O, and there's no self-healing involved --
	 * that is, we're just resilvering what we expect to resilver --
	 * then don't do the I/O unless zio's txg is actually in vd's DTL.
	 * This prevents spurious resilvering.
	 *
	 * There are a few ways that we can end up creating these spurious
	 * resilver i/os:
	 *
	 * 1. A resilver i/o will be issued if any DVA in the BP has a
	 * dirty DTL.  The mirror code will issue resilver writes to
	 * each DVA, including the one(s) that are not on vdevs with dirty
	 * DTLs.
	 *
	 * 2. With nested replication, which happens when we have a
	 * "replacing" or "spare" vdev that's a child of a mirror or raidz.
	 * For example, given mirror(replacing(A+B), C), it's likely that
	 * only A is out of date (it's the new device). In this case, we'll
	 * read from C, then use the data to resilver A+B -- but we don't
	 * actually want to resilver B, just A. The top-level mirror has no
	 * way to know this, so instead we just discard unnecessary repairs
	 * as we work our way down the vdev tree.
	 *
	 * 3. ZTEST also creates mirrors of mirrors, mirrors of raidz, etc.
	 * The same logic applies to any form of nested replication: ditto
	 * + mirror, RAID-Z + replacing, etc.
	 *
	 * However, indirect vdevs point off to other vdevs which may have
	 * DTL's, so we never bypass them.  The child i/os on concrete vdevs
	 * will be properly bypassed instead.
	 *
	 * Leaf DTL_PARTIAL can be empty when a legitimate write comes from
	 * a dRAID spare vdev. For example, when a dRAID spare is first
	 * used, its spare blocks need to be written to but the leaf vdev's
	 * of such blocks can have empty DTL_PARTIAL.
	 *
	 * There seemed no clean way to allow such writes while bypassing
	 * spurious ones. At this point, just avoid all bypassing for dRAID
	 * for correctness.
	 */
	if ((zio->io_flags & ZIO_FLAG_IO_REPAIR) &&
	    !(zio->io_flags & ZIO_FLAG_SELF_HEAL) &&
	    zio->io_txg != 0 &&	/* not a delegated i/o */
	    vd->vdev_ops != &vdev_indirect_ops &&
	    vd->vdev_top->vdev_ops != &vdev_draid_ops &&
	    !vdev_dtl_contains(vd, DTL_PARTIAL, zio->io_txg, 1)) {
		ASSERT(zio->io_type == ZIO_TYPE_WRITE);
		zio_vdev_io_bypass(zio);
		return (zio);
	}

	/*
	 * Select the next best leaf I/O to process.  Distributed spares are
	 * excluded since they dispatch the I/O directly to a leaf vdev after
	 * applying the dRAID mapping.
	 */
	if (vd->vdev_ops->vdev_op_leaf &&
	    vd->vdev_ops != &vdev_draid_spare_ops &&
	    (zio->io_type == ZIO_TYPE_READ ||
	    zio->io_type == ZIO_TYPE_WRITE ||
	    zio->io_type == ZIO_TYPE_TRIM)) {

		if ((zio = vdev_queue_io(zio)) == NULL)
			return (NULL);

		if (!vdev_accessible(vd, zio)) {
			zio->io_error = SET_ERROR(ENXIO);
			zio_interrupt(zio);
			return (NULL);
		}
		zio->io_delay = gethrtime();

		if (zio_handle_device_injection(vd, zio, ENOSYS) != 0) {
			/*
			 * "no-op" injections return success, but do no actual
			 * work. Just return it.
			 */
			zio_delay_interrupt(zio);
			return (NULL);
		}
	}

	vd->vdev_ops->vdev_op_io_start(zio);
	return (NULL);
}

static zio_t *
zio_vdev_io_done(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_ops_t *ops = vd ? vd->vdev_ops : &vdev_mirror_ops;
	boolean_t unexpected_error = B_FALSE;

	if (zio_wait_for_children(zio, ZIO_CHILD_VDEV_BIT, ZIO_WAIT_DONE)) {
		return (NULL);
	}

	ASSERT(zio->io_type == ZIO_TYPE_READ ||
	    zio->io_type == ZIO_TYPE_WRITE ||
	    zio->io_type == ZIO_TYPE_FLUSH ||
	    zio->io_type == ZIO_TYPE_TRIM);

	if (zio->io_delay)
		zio->io_delay = gethrtime() - zio->io_delay;

	if (vd != NULL && vd->vdev_ops->vdev_op_leaf &&
	    vd->vdev_ops != &vdev_draid_spare_ops) {
		if (zio->io_type != ZIO_TYPE_FLUSH)
			vdev_queue_io_done(zio);

		if (zio_injection_enabled && zio->io_error == 0)
			zio->io_error = zio_handle_device_injections(vd, zio,
			    EIO, EILSEQ);

		if (zio_injection_enabled && zio->io_error == 0)
			zio->io_error = zio_handle_label_injection(zio, EIO);

		if (zio->io_error && zio->io_type != ZIO_TYPE_FLUSH &&
		    zio->io_type != ZIO_TYPE_TRIM) {
			if (!vdev_accessible(vd, zio)) {
				zio->io_error = SET_ERROR(ENXIO);
			} else {
				unexpected_error = B_TRUE;
			}
		}
	}

	ops->vdev_op_io_done(zio);

	if (unexpected_error && vd->vdev_remove_wanted == B_FALSE)
		VERIFY(vdev_probe(vd, zio) == NULL);

	return (zio);
}

/*
 * This function is used to change the priority of an existing zio that is
 * currently in-flight. This is used by the arc to upgrade priority in the
 * event that a demand read is made for a block that is currently queued
 * as a scrub or async read IO. Otherwise, the high priority read request
 * would end up having to wait for the lower priority IO.
 */
void
zio_change_priority(zio_t *pio, zio_priority_t priority)
{
	zio_t *cio, *cio_next;
	zio_link_t *zl = NULL;

	ASSERT3U(priority, <, ZIO_PRIORITY_NUM_QUEUEABLE);

	if (pio->io_vd != NULL && pio->io_vd->vdev_ops->vdev_op_leaf) {
		vdev_queue_change_io_priority(pio, priority);
	} else {
		pio->io_priority = priority;
	}

	mutex_enter(&pio->io_lock);
	for (cio = zio_walk_children(pio, &zl); cio != NULL; cio = cio_next) {
		cio_next = zio_walk_children(pio, &zl);
		zio_change_priority(cio, priority);
	}
	mutex_exit(&pio->io_lock);
}

/*
 * For non-raidz ZIOs, we can just copy aside the bad data read from the
 * disk, and use that to finish the checksum ereport later.
 */
static void
zio_vsd_default_cksum_finish(zio_cksum_report_t *zcr,
    const abd_t *good_buf)
{
	/* no processing needed */
	zfs_ereport_finish_checksum(zcr, good_buf, zcr->zcr_cbdata, B_FALSE);
}

void
zio_vsd_default_cksum_report(zio_t *zio, zio_cksum_report_t *zcr)
{
	void *abd = abd_alloc_sametype(zio->io_abd, zio->io_size);

	abd_copy(abd, zio->io_abd, zio->io_size);

	zcr->zcr_cbinfo = zio->io_size;
	zcr->zcr_cbdata = abd;
	zcr->zcr_finish = zio_vsd_default_cksum_finish;
	zcr->zcr_free = zio_abd_free;
}

static zio_t *
zio_vdev_io_assess(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;

	if (zio_wait_for_children(zio, ZIO_CHILD_VDEV_BIT, ZIO_WAIT_DONE)) {
		return (NULL);
	}

	if (vd == NULL && !(zio->io_flags & ZIO_FLAG_CONFIG_WRITER))
		spa_config_exit(zio->io_spa, SCL_ZIO, zio);

	if (zio->io_vsd != NULL) {
		zio->io_vsd_ops->vsd_free(zio);
		zio->io_vsd = NULL;
	}

	/*
	 * If a Direct I/O operation has a checksum verify error then this I/O
	 * should not attempt to be issued again.
	 */
	if (zio->io_post & ZIO_POST_DIO_CHKSUM_ERR) {
		if (zio->io_type == ZIO_TYPE_WRITE) {
			ASSERT3U(zio->io_child_type, ==, ZIO_CHILD_LOGICAL);
			ASSERT3U(zio->io_error, ==, EIO);
		}
		zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;
		return (zio);
	}

	if (zio_injection_enabled && zio->io_error == 0)
		zio->io_error = zio_handle_fault_injection(zio, EIO);

	/*
	 * If the I/O failed, determine whether we should attempt to retry it.
	 *
	 * On retry, we cut in line in the issue queue, since we don't want
	 * compression/checksumming/etc. work to prevent our (cheap) IO reissue.
	 */
	if (zio->io_error && vd == NULL &&
	    !(zio->io_flags & (ZIO_FLAG_DONT_RETRY | ZIO_FLAG_IO_RETRY))) {
		ASSERT(!(zio->io_flags & ZIO_FLAG_DONT_QUEUE));	/* not a leaf */
		ASSERT(!(zio->io_flags & ZIO_FLAG_IO_BYPASS));	/* not a leaf */
		zio->io_error = 0;
		zio->io_flags |= ZIO_FLAG_IO_RETRY | ZIO_FLAG_DONT_AGGREGATE;
		zio->io_stage = ZIO_STAGE_VDEV_IO_START >> 1;
		zio_taskq_dispatch(zio, ZIO_TASKQ_ISSUE,
		    zio_requeue_io_start_cut_in_line);
		return (NULL);
	}

	/*
	 * If we got an error on a leaf device, convert it to ENXIO
	 * if the device is not accessible at all.
	 */
	if (zio->io_error && vd != NULL && vd->vdev_ops->vdev_op_leaf &&
	    !vdev_accessible(vd, zio))
		zio->io_error = SET_ERROR(ENXIO);

	/*
	 * If we can't write to an interior vdev (mirror or RAID-Z),
	 * set vdev_cant_write so that we stop trying to allocate from it.
	 */
	if (zio->io_error == ENXIO && zio->io_type == ZIO_TYPE_WRITE &&
	    vd != NULL && !vd->vdev_ops->vdev_op_leaf) {
		vdev_dbgmsg(vd, "zio_vdev_io_assess(zio=%px) setting "
		    "cant_write=TRUE due to write failure with ENXIO",
		    zio);
		vd->vdev_cant_write = B_TRUE;
	}

	/*
	 * If a cache flush returns ENOTSUP we know that no future
	 * attempts will ever succeed. In this case we set a persistent
	 * boolean flag so that we don't bother with it in the future, and
	 * then we act like the flush succeeded.
	 */
	if (zio->io_error == ENOTSUP && zio->io_type == ZIO_TYPE_FLUSH &&
	    vd != NULL) {
		vd->vdev_nowritecache = B_TRUE;
		zio->io_error = 0;
	}

	if (zio->io_error)
		zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;

	return (zio);
}

void
zio_vdev_io_reissue(zio_t *zio)
{
	ASSERT(zio->io_stage == ZIO_STAGE_VDEV_IO_START);
	ASSERT(zio->io_error == 0);

	zio->io_stage >>= 1;
}

void
zio_vdev_io_redone(zio_t *zio)
{
	ASSERT(zio->io_stage == ZIO_STAGE_VDEV_IO_DONE);

	zio->io_stage >>= 1;
}

void
zio_vdev_io_bypass(zio_t *zio)
{
	ASSERT(zio->io_stage == ZIO_STAGE_VDEV_IO_START);
	ASSERT(zio->io_error == 0);

	zio->io_flags |= ZIO_FLAG_IO_BYPASS;
	zio->io_stage = ZIO_STAGE_VDEV_IO_ASSESS >> 1;
}

/*
 * ==========================================================================
 * Encrypt and store encryption parameters
 * ==========================================================================
 */


/*
 * This function is used for ZIO_STAGE_ENCRYPT. It is responsible for
 * managing the storage of encryption parameters and passing them to the
 * lower-level encryption functions.
 */
static zio_t *
zio_encrypt(zio_t *zio)
{
	zio_prop_t *zp = &zio->io_prop;
	spa_t *spa = zio->io_spa;
	blkptr_t *bp = zio->io_bp;
	uint64_t psize = BP_GET_PSIZE(bp);
	uint64_t dsobj = zio->io_bookmark.zb_objset;
	dmu_object_type_t ot = BP_GET_TYPE(bp);
	void *enc_buf = NULL;
	abd_t *eabd = NULL;
	uint8_t salt[ZIO_DATA_SALT_LEN];
	uint8_t iv[ZIO_DATA_IV_LEN];
	uint8_t mac[ZIO_DATA_MAC_LEN];
	boolean_t no_crypt = B_FALSE;

	/* the root zio already encrypted the data */
	if (zio->io_child_type == ZIO_CHILD_GANG)
		return (zio);

	/* only ZIL blocks are re-encrypted on rewrite */
	if (!IO_IS_ALLOCATING(zio) && ot != DMU_OT_INTENT_LOG)
		return (zio);

	if (!(zp->zp_encrypt || BP_IS_ENCRYPTED(bp))) {
		BP_SET_CRYPT(bp, B_FALSE);
		return (zio);
	}

	/* if we are doing raw encryption set the provided encryption params */
	if (zio->io_flags & ZIO_FLAG_RAW_ENCRYPT) {
		ASSERT0(BP_GET_LEVEL(bp));
		BP_SET_CRYPT(bp, B_TRUE);
		BP_SET_BYTEORDER(bp, zp->zp_byteorder);
		if (ot != DMU_OT_OBJSET)
			zio_crypt_encode_mac_bp(bp, zp->zp_mac);

		/* dnode blocks must be written out in the provided byteorder */
		if (zp->zp_byteorder != ZFS_HOST_BYTEORDER &&
		    ot == DMU_OT_DNODE) {
			void *bswap_buf = zio_buf_alloc(psize);
			abd_t *babd = abd_get_from_buf(bswap_buf, psize);

			ASSERT3U(BP_GET_COMPRESS(bp), ==, ZIO_COMPRESS_OFF);
			abd_copy_to_buf(bswap_buf, zio->io_abd, psize);
			dmu_ot_byteswap[DMU_OT_BYTESWAP(ot)].ob_func(bswap_buf,
			    psize);

			abd_take_ownership_of_buf(babd, B_TRUE);
			zio_push_transform(zio, babd, psize, psize, NULL);
		}

		if (DMU_OT_IS_ENCRYPTED(ot))
			zio_crypt_encode_params_bp(bp, zp->zp_salt, zp->zp_iv);
		return (zio);
	}

	/* indirect blocks only maintain a cksum of the lower level MACs */
	if (BP_GET_LEVEL(bp) > 0) {
		BP_SET_CRYPT(bp, B_TRUE);
		VERIFY0(zio_crypt_do_indirect_mac_checksum_abd(B_TRUE,
		    zio->io_orig_abd, BP_GET_LSIZE(bp), BP_SHOULD_BYTESWAP(bp),
		    mac));
		zio_crypt_encode_mac_bp(bp, mac);
		return (zio);
	}

	/*
	 * Objset blocks are a special case since they have 2 256-bit MACs
	 * embedded within them.
	 */
	if (ot == DMU_OT_OBJSET) {
		ASSERT0(DMU_OT_IS_ENCRYPTED(ot));
		ASSERT3U(BP_GET_COMPRESS(bp), ==, ZIO_COMPRESS_OFF);
		BP_SET_CRYPT(bp, B_TRUE);
		VERIFY0(spa_do_crypt_objset_mac_abd(B_TRUE, spa, dsobj,
		    zio->io_abd, psize, BP_SHOULD_BYTESWAP(bp)));
		return (zio);
	}

	/* unencrypted object types are only authenticated with a MAC */
	if (!DMU_OT_IS_ENCRYPTED(ot)) {
		BP_SET_CRYPT(bp, B_TRUE);
		VERIFY0(spa_do_crypt_mac_abd(B_TRUE, spa, dsobj,
		    zio->io_abd, psize, mac));
		zio_crypt_encode_mac_bp(bp, mac);
		return (zio);
	}

	/*
	 * Later passes of sync-to-convergence may decide to rewrite data
	 * in place to avoid more disk reallocations. This presents a problem
	 * for encryption because this constitutes rewriting the new data with
	 * the same encryption key and IV. However, this only applies to blocks
	 * in the MOS (particularly the spacemaps) and we do not encrypt the
	 * MOS. We assert that the zio is allocating or an intent log write
	 * to enforce this.
	 */
	ASSERT(IO_IS_ALLOCATING(zio) || ot == DMU_OT_INTENT_LOG);
	ASSERT(BP_GET_LEVEL(bp) == 0 || ot == DMU_OT_INTENT_LOG);
	ASSERT(spa_feature_is_active(spa, SPA_FEATURE_ENCRYPTION));
	ASSERT3U(psize, !=, 0);

	enc_buf = zio_buf_alloc(psize);
	eabd = abd_get_from_buf(enc_buf, psize);
	abd_take_ownership_of_buf(eabd, B_TRUE);

	/*
	 * For an explanation of what encryption parameters are stored
	 * where, see the block comment in zio_crypt.c.
	 */
	if (ot == DMU_OT_INTENT_LOG) {
		zio_crypt_decode_params_bp(bp, salt, iv);
	} else {
		BP_SET_CRYPT(bp, B_TRUE);
	}

	/* Perform the encryption. This should not fail */
	VERIFY0(spa_do_crypt_abd(B_TRUE, spa, &zio->io_bookmark,
	    BP_GET_TYPE(bp), BP_GET_DEDUP(bp), BP_SHOULD_BYTESWAP(bp),
	    salt, iv, mac, psize, zio->io_abd, eabd, &no_crypt));

	/* encode encryption metadata into the bp */
	if (ot == DMU_OT_INTENT_LOG) {
		/*
		 * ZIL blocks store the MAC in the embedded checksum, so the
		 * transform must always be applied.
		 */
		zio_crypt_encode_mac_zil(enc_buf, mac);
		zio_push_transform(zio, eabd, psize, psize, NULL);
	} else {
		BP_SET_CRYPT(bp, B_TRUE);
		zio_crypt_encode_params_bp(bp, salt, iv);
		zio_crypt_encode_mac_bp(bp, mac);

		if (no_crypt) {
			ASSERT3U(ot, ==, DMU_OT_DNODE);
			abd_free(eabd);
		} else {
			zio_push_transform(zio, eabd, psize, psize, NULL);
		}
	}

	return (zio);
}

/*
 * ==========================================================================
 * Generate and verify checksums
 * ==========================================================================
 */
static zio_t *
zio_checksum_generate(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	enum zio_checksum checksum;

	if (bp == NULL) {
		/*
		 * This is zio_write_phys().
		 * We're either generating a label checksum, or none at all.
		 */
		checksum = zio->io_prop.zp_checksum;

		if (checksum == ZIO_CHECKSUM_OFF)
			return (zio);

		ASSERT(checksum == ZIO_CHECKSUM_LABEL);
	} else {
		if (BP_IS_GANG(bp) && zio->io_child_type == ZIO_CHILD_GANG) {
			ASSERT(!IO_IS_ALLOCATING(zio));
			checksum = ZIO_CHECKSUM_GANG_HEADER;
		} else {
			checksum = BP_GET_CHECKSUM(bp);
		}
	}

	zio_checksum_compute(zio, checksum, zio->io_abd, zio->io_size);

	return (zio);
}

static zio_t *
zio_checksum_verify(zio_t *zio)
{
	zio_bad_cksum_t info;
	blkptr_t *bp = zio->io_bp;
	int error;

	ASSERT(zio->io_vd != NULL);

	if (bp == NULL) {
		/*
		 * This is zio_read_phys().
		 * We're either verifying a label checksum, or nothing at all.
		 */
		if (zio->io_prop.zp_checksum == ZIO_CHECKSUM_OFF)
			return (zio);

		ASSERT3U(zio->io_prop.zp_checksum, ==, ZIO_CHECKSUM_LABEL);
	}

	ASSERT0(zio->io_post & ZIO_POST_DIO_CHKSUM_ERR);
	IMPLY(zio->io_flags & ZIO_FLAG_DIO_READ,
	    !(zio->io_flags & ZIO_FLAG_SPECULATIVE));

	if ((error = zio_checksum_error(zio, &info)) != 0) {
		zio->io_error = error;
		if (error == ECKSUM &&
		    !(zio->io_flags & ZIO_FLAG_SPECULATIVE)) {
			if (zio->io_flags & ZIO_FLAG_DIO_READ) {
				zio->io_post |= ZIO_POST_DIO_CHKSUM_ERR;
				zio_t *pio = zio_unique_parent(zio);
				/*
				 * Any Direct I/O read that has a checksum
				 * error must be treated as suspicous as the
				 * contents of the buffer could be getting
				 * manipulated while the I/O is taking place.
				 *
				 * The checksum verify error will only be
				 * reported here for disk and file VDEV's and
				 * will be reported on those that the failure
				 * occurred on. Other types of VDEV's report the
				 * verify failure in their own code paths.
				 */
				if (pio->io_child_type == ZIO_CHILD_LOGICAL) {
					zio_dio_chksum_verify_error_report(zio);
				}
			} else {
				mutex_enter(&zio->io_vd->vdev_stat_lock);
				zio->io_vd->vdev_stat.vs_checksum_errors++;
				mutex_exit(&zio->io_vd->vdev_stat_lock);
				(void) zfs_ereport_start_checksum(zio->io_spa,
				    zio->io_vd, &zio->io_bookmark, zio,
				    zio->io_offset, zio->io_size, &info);
			}
		}
	}

	return (zio);
}

static zio_t *
zio_dio_checksum_verify(zio_t *zio)
{
	zio_t *pio = zio_unique_parent(zio);
	int error;

	ASSERT3P(zio->io_vd, !=, NULL);
	ASSERT3P(zio->io_bp, !=, NULL);
	ASSERT3U(zio->io_child_type, ==, ZIO_CHILD_VDEV);
	ASSERT3U(zio->io_type, ==, ZIO_TYPE_WRITE);
	ASSERT3B(pio->io_prop.zp_direct_write, ==, B_TRUE);
	ASSERT3U(pio->io_child_type, ==, ZIO_CHILD_LOGICAL);

	if (zfs_vdev_direct_write_verify == 0 || zio->io_error != 0)
		goto out;

	if ((error = zio_checksum_error(zio, NULL)) != 0) {
		zio->io_error = error;
		if (error == ECKSUM) {
			zio->io_post |= ZIO_POST_DIO_CHKSUM_ERR;
			zio_dio_chksum_verify_error_report(zio);
		}
	}

out:
	return (zio);
}


/*
 * Called by RAID-Z to ensure we don't compute the checksum twice.
 */
void
zio_checksum_verified(zio_t *zio)
{
	zio->io_pipeline &= ~ZIO_STAGE_CHECKSUM_VERIFY;
}

/*
 * Report Direct I/O checksum verify error and create ZED event.
 */
void
zio_dio_chksum_verify_error_report(zio_t *zio)
{
	ASSERT(zio->io_post & ZIO_POST_DIO_CHKSUM_ERR);

	if (zio->io_child_type == ZIO_CHILD_LOGICAL)
		return;

	mutex_enter(&zio->io_vd->vdev_stat_lock);
	zio->io_vd->vdev_stat.vs_dio_verify_errors++;
	mutex_exit(&zio->io_vd->vdev_stat_lock);
	if (zio->io_type == ZIO_TYPE_WRITE) {
		/*
		 * Convert checksum error for writes into EIO.
		 */
		zio->io_error = SET_ERROR(EIO);
		/*
		 * Report dio_verify_wr ZED event.
		 */
		(void) zfs_ereport_post(FM_EREPORT_ZFS_DIO_VERIFY_WR,
		    zio->io_spa,  zio->io_vd, &zio->io_bookmark, zio, 0);
	} else {
		/*
		 * Report dio_verify_rd ZED event.
		 */
		(void) zfs_ereport_post(FM_EREPORT_ZFS_DIO_VERIFY_RD,
		    zio->io_spa, zio->io_vd, &zio->io_bookmark, zio, 0);
	}
}

/*
 * ==========================================================================
 * Error rank.  Error are ranked in the order 0, ENXIO, ECKSUM, EIO, other.
 * An error of 0 indicates success.  ENXIO indicates whole-device failure,
 * which may be transient (e.g. unplugged) or permanent.  ECKSUM and EIO
 * indicate errors that are specific to one I/O, and most likely permanent.
 * Any other error is presumed to be worse because we weren't expecting it.
 * ==========================================================================
 */
int
zio_worst_error(int e1, int e2)
{
	static int zio_error_rank[] = { 0, ENXIO, ECKSUM, EIO };
	int r1, r2;

	for (r1 = 0; r1 < sizeof (zio_error_rank) / sizeof (int); r1++)
		if (e1 == zio_error_rank[r1])
			break;

	for (r2 = 0; r2 < sizeof (zio_error_rank) / sizeof (int); r2++)
		if (e2 == zio_error_rank[r2])
			break;

	return (r1 > r2 ? e1 : e2);
}

/*
 * ==========================================================================
 * I/O completion
 * ==========================================================================
 */
static zio_t *
zio_ready(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	zio_t *pio, *pio_next;
	zio_link_t *zl = NULL;

	if (zio_wait_for_children(zio, ZIO_CHILD_LOGICAL_BIT |
	    ZIO_CHILD_GANG_BIT | ZIO_CHILD_DDT_BIT, ZIO_WAIT_READY)) {
		return (NULL);
	}

	if (zio->io_ready) {
		ASSERT(IO_IS_ALLOCATING(zio));
		ASSERT(BP_GET_LOGICAL_BIRTH(bp) == zio->io_txg ||
		    BP_IS_HOLE(bp) || (zio->io_flags & ZIO_FLAG_NOPWRITE));
		ASSERT(zio->io_children[ZIO_CHILD_GANG][ZIO_WAIT_READY] == 0);

		zio->io_ready(zio);
	}

#ifdef ZFS_DEBUG
	if (bp != NULL && bp != &zio->io_bp_copy)
		zio->io_bp_copy = *bp;
#endif

	if (zio->io_error != 0) {
		zio->io_pipeline = ZIO_INTERLOCK_PIPELINE;

		if (zio->io_flags & ZIO_FLAG_ALLOC_THROTTLED) {
			ASSERT(IO_IS_ALLOCATING(zio));
			ASSERT(zio->io_priority == ZIO_PRIORITY_ASYNC_WRITE);
			ASSERT(zio->io_metaslab_class != NULL);
			ASSERT(ZIO_HAS_ALLOCATOR(zio));

			/*
			 * We were unable to allocate anything, unreserve and
			 * issue the next I/O to allocate.
			 */
			if (metaslab_class_throttle_unreserve(
			    zio->io_metaslab_class, zio->io_allocator,
			    zio->io_prop.zp_copies, zio->io_size)) {
				zio_allocate_dispatch(zio->io_metaslab_class,
				    zio->io_allocator);
			}
		}
	}

	mutex_enter(&zio->io_lock);
	zio->io_state[ZIO_WAIT_READY] = 1;
	pio = zio_walk_parents(zio, &zl);
	mutex_exit(&zio->io_lock);

	/*
	 * As we notify zio's parents, new parents could be added.
	 * New parents go to the head of zio's io_parent_list, however,
	 * so we will (correctly) not notify them.  The remainder of zio's
	 * io_parent_list, from 'pio_next' onward, cannot change because
	 * all parents must wait for us to be done before they can be done.
	 */
	for (; pio != NULL; pio = pio_next) {
		pio_next = zio_walk_parents(zio, &zl);
		zio_notify_parent(pio, zio, ZIO_WAIT_READY, NULL);
	}

	if (zio->io_flags & ZIO_FLAG_NODATA) {
		if (bp != NULL && BP_IS_GANG(bp)) {
			zio->io_flags &= ~ZIO_FLAG_NODATA;
		} else {
			ASSERT((uintptr_t)zio->io_abd < SPA_MAXBLOCKSIZE);
			zio->io_pipeline &= ~ZIO_VDEV_IO_STAGES;
		}
	}

	if (zio_injection_enabled &&
	    zio->io_spa->spa_syncing_txg == zio->io_txg)
		zio_handle_ignored_writes(zio);

	return (zio);
}

/*
 * Update the allocation throttle accounting.
 */
static void
zio_dva_throttle_done(zio_t *zio)
{
	zio_t *pio = zio_unique_parent(zio);
	vdev_t *vd = zio->io_vd;
	int flags = METASLAB_ASYNC_ALLOC;
	const void *tag = pio;
	uint64_t size = pio->io_size;

	ASSERT3P(zio->io_bp, !=, NULL);
	ASSERT3U(zio->io_type, ==, ZIO_TYPE_WRITE);
	ASSERT3U(zio->io_priority, ==, ZIO_PRIORITY_ASYNC_WRITE);
	ASSERT3U(zio->io_child_type, ==, ZIO_CHILD_VDEV);
	ASSERT(vd != NULL);
	ASSERT3P(vd, ==, vd->vdev_top);
	ASSERT(zio_injection_enabled || !(zio->io_flags & ZIO_FLAG_IO_RETRY));
	ASSERT(!(zio->io_flags & ZIO_FLAG_IO_REPAIR));
	ASSERT(zio->io_flags & ZIO_FLAG_ALLOC_THROTTLED);

	/*
	 * Parents of gang children can have two flavors -- ones that allocated
	 * the gang header (will have ZIO_FLAG_IO_REWRITE set) and ones that
	 * allocated the constituent blocks.  The first use their parent as tag.
	 * We set the size to match the original allocation call for that case.
	 */
	if (pio->io_child_type == ZIO_CHILD_GANG &&
	    (pio->io_flags & ZIO_FLAG_IO_REWRITE)) {
		tag = zio_unique_parent(pio);
		size = SPA_OLD_GANGBLOCKSIZE;
	}

	ASSERT(IO_IS_ALLOCATING(pio) || (pio->io_child_type == ZIO_CHILD_GANG &&
	    (pio->io_flags & ZIO_FLAG_IO_REWRITE)));
	ASSERT(ZIO_HAS_ALLOCATOR(pio));
	ASSERT3P(zio, !=, zio->io_logical);
	ASSERT(zio->io_logical != NULL);
	ASSERT(!(zio->io_flags & ZIO_FLAG_IO_REPAIR));
	ASSERT0(zio->io_flags & ZIO_FLAG_NOPWRITE);
	ASSERT(zio->io_metaslab_class != NULL);
	ASSERT(zio->io_metaslab_class->mc_alloc_throttle_enabled);

	metaslab_group_alloc_decrement(zio->io_spa, vd->vdev_id,
	    pio->io_allocator, flags, size, tag);

	if (metaslab_class_throttle_unreserve(pio->io_metaslab_class,
	    pio->io_allocator, 1, pio->io_size)) {
		zio_allocate_dispatch(zio->io_metaslab_class,
		    pio->io_allocator);
	}
}

static zio_t *
zio_done(zio_t *zio)
{
	/*
	 * Always attempt to keep stack usage minimal here since
	 * we can be called recursively up to 19 levels deep.
	 */
	const uint64_t psize = zio->io_size;
	zio_t *pio, *pio_next;
	zio_link_t *zl = NULL;

	/*
	 * If our children haven't all completed,
	 * wait for them and then repeat this pipeline stage.
	 */
	if (zio_wait_for_children(zio, ZIO_CHILD_ALL_BITS, ZIO_WAIT_DONE)) {
		return (NULL);
	}

	/*
	 * If the allocation throttle is enabled, then update the accounting.
	 * We only track child I/Os that are part of an allocating async
	 * write. We must do this since the allocation is performed
	 * by the logical I/O but the actual write is done by child I/Os.
	 */
	if (zio->io_flags & ZIO_FLAG_ALLOC_THROTTLED &&
	    zio->io_child_type == ZIO_CHILD_VDEV)
		zio_dva_throttle_done(zio);

	for (int c = 0; c < ZIO_CHILD_TYPES; c++)
		for (int w = 0; w < ZIO_WAIT_TYPES; w++)
			ASSERT(zio->io_children[c][w] == 0);

	if (zio->io_bp != NULL && !BP_IS_EMBEDDED(zio->io_bp)) {
		ASSERT(zio->io_bp->blk_pad[0] == 0);
		ASSERT(zio->io_bp->blk_pad[1] == 0);
		ASSERT(memcmp(zio->io_bp, &zio->io_bp_copy,
		    sizeof (blkptr_t)) == 0 ||
		    (zio->io_bp == zio_unique_parent(zio)->io_bp));
		if (zio->io_type == ZIO_TYPE_WRITE && !BP_IS_HOLE(zio->io_bp) &&
		    zio->io_bp_override == NULL &&
		    !(zio->io_flags & ZIO_FLAG_IO_REPAIR)) {
			ASSERT3U(zio->io_prop.zp_copies, <=,
			    BP_GET_NDVAS(zio->io_bp));
			ASSERT(BP_COUNT_GANG(zio->io_bp) == 0 ||
			    (BP_COUNT_GANG(zio->io_bp) ==
			    BP_GET_NDVAS(zio->io_bp)));
		}
		if (zio->io_flags & ZIO_FLAG_NOPWRITE)
			VERIFY(BP_EQUAL(zio->io_bp, &zio->io_bp_orig));
	}

	/*
	 * If there were child vdev/gang/ddt errors, they apply to us now.
	 */
	zio_inherit_child_errors(zio, ZIO_CHILD_VDEV);
	zio_inherit_child_errors(zio, ZIO_CHILD_GANG);
	zio_inherit_child_errors(zio, ZIO_CHILD_DDT);

	/*
	 * If the I/O on the transformed data was successful, generate any
	 * checksum reports now while we still have the transformed data.
	 */
	if (zio->io_error == 0) {
		while (zio->io_cksum_report != NULL) {
			zio_cksum_report_t *zcr = zio->io_cksum_report;
			uint64_t align = zcr->zcr_align;
			uint64_t asize = P2ROUNDUP(psize, align);
			abd_t *adata = zio->io_abd;

			if (adata != NULL && asize != psize) {
				adata = abd_alloc(asize, B_TRUE);
				abd_copy(adata, zio->io_abd, psize);
				abd_zero_off(adata, psize, asize - psize);
			}

			zio->io_cksum_report = zcr->zcr_next;
			zcr->zcr_next = NULL;
			zcr->zcr_finish(zcr, adata);
			zfs_ereport_free_checksum(zcr);

			if (adata != NULL && asize != psize)
				abd_free(adata);
		}
	}

	zio_pop_transforms(zio);	/* note: may set zio->io_error */

	vdev_stat_update(zio, psize);

	/*
	 * If this I/O is attached to a particular vdev is slow, exceeding
	 * 30 seconds to complete, post an error described the I/O delay.
	 * We ignore these errors if the device is currently unavailable.
	 */
	if (zio->io_delay >= MSEC2NSEC(zio_slow_io_ms)) {
		if (zio->io_vd != NULL && !vdev_is_dead(zio->io_vd)) {
			/*
			 * We want to only increment our slow IO counters if
			 * the IO is valid (i.e. not if the drive is removed).
			 *
			 * zfs_ereport_post() will also do these checks, but
			 * it can also ratelimit and have other failures, so we
			 * need to increment the slow_io counters independent
			 * of it.
			 */
			if (zfs_ereport_is_valid(FM_EREPORT_ZFS_DELAY,
			    zio->io_spa, zio->io_vd, zio)) {
				mutex_enter(&zio->io_vd->vdev_stat_lock);
				zio->io_vd->vdev_stat.vs_slow_ios++;
				mutex_exit(&zio->io_vd->vdev_stat_lock);

				(void) zfs_ereport_post(FM_EREPORT_ZFS_DELAY,
				    zio->io_spa, zio->io_vd, &zio->io_bookmark,
				    zio, 0);
			}
		}
	}

	if (zio->io_error) {
		/*
		 * If this I/O is attached to a particular vdev,
		 * generate an error message describing the I/O failure
		 * at the block level.  We ignore these errors if the
		 * device is currently unavailable.
		 */
		if (zio->io_error != ECKSUM && zio->io_vd != NULL &&
		    !vdev_is_dead(zio->io_vd) &&
		    !(zio->io_post & ZIO_POST_DIO_CHKSUM_ERR)) {
			int ret = zfs_ereport_post(FM_EREPORT_ZFS_IO,
			    zio->io_spa, zio->io_vd, &zio->io_bookmark, zio, 0);
			if (ret != EALREADY) {
				mutex_enter(&zio->io_vd->vdev_stat_lock);
				if (zio->io_type == ZIO_TYPE_READ)
					zio->io_vd->vdev_stat.vs_read_errors++;
				else if (zio->io_type == ZIO_TYPE_WRITE)
					zio->io_vd->vdev_stat.vs_write_errors++;
				mutex_exit(&zio->io_vd->vdev_stat_lock);
			}
		}

		if ((zio->io_error == EIO || !(zio->io_flags &
		    (ZIO_FLAG_SPECULATIVE | ZIO_FLAG_DONT_PROPAGATE))) &&
		    !(zio->io_post & ZIO_POST_DIO_CHKSUM_ERR) &&
		    zio == zio->io_logical) {
			/*
			 * For logical I/O requests, tell the SPA to log the
			 * error and generate a logical data ereport.
			 */
			spa_log_error(zio->io_spa, &zio->io_bookmark,
			    BP_GET_LOGICAL_BIRTH(zio->io_bp));
			(void) zfs_ereport_post(FM_EREPORT_ZFS_DATA,
			    zio->io_spa, NULL, &zio->io_bookmark, zio, 0);
		}
	}

	if (zio->io_error && zio == zio->io_logical) {

		/*
		 * A DDT child tried to create a mixed gang/non-gang BP. We're
		 * going to have to just retry as a non-dedup IO.
		 */
		if (zio->io_error == EAGAIN && IO_IS_ALLOCATING(zio) &&
		    zio->io_prop.zp_dedup) {
			zio->io_post |= ZIO_POST_REEXECUTE;
			zio->io_prop.zp_dedup = B_FALSE;
		}
		/*
		 * Determine whether zio should be reexecuted.  This will
		 * propagate all the way to the root via zio_notify_parent().
		 */
		ASSERT(zio->io_vd == NULL && zio->io_bp != NULL);
		ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);

		if (IO_IS_ALLOCATING(zio) &&
		    !(zio->io_flags & ZIO_FLAG_CANFAIL) &&
		    !(zio->io_post & ZIO_POST_DIO_CHKSUM_ERR)) {
			if (zio->io_error != ENOSPC)
				zio->io_post |= ZIO_POST_REEXECUTE;
			else
				zio->io_post |= ZIO_POST_SUSPEND;
		}

		if ((zio->io_type == ZIO_TYPE_READ ||
		    zio->io_type == ZIO_TYPE_FREE) &&
		    !(zio->io_flags & ZIO_FLAG_SCAN_THREAD) &&
		    zio->io_error == ENXIO &&
		    spa_load_state(zio->io_spa) == SPA_LOAD_NONE &&
		    spa_get_failmode(zio->io_spa) != ZIO_FAILURE_MODE_CONTINUE)
			zio->io_post |= ZIO_POST_SUSPEND;

		if (!(zio->io_flags & ZIO_FLAG_CANFAIL) &&
		    !(zio->io_post & (ZIO_POST_REEXECUTE|ZIO_POST_SUSPEND)))
			zio->io_post |= ZIO_POST_SUSPEND;

		/*
		 * Here is a possibly good place to attempt to do
		 * either combinatorial reconstruction or error correction
		 * based on checksums.  It also might be a good place
		 * to send out preliminary ereports before we suspend
		 * processing.
		 */
	}

	/*
	 * If there were logical child errors, they apply to us now.
	 * We defer this until now to avoid conflating logical child
	 * errors with errors that happened to the zio itself when
	 * updating vdev stats and reporting FMA events above.
	 */
	zio_inherit_child_errors(zio, ZIO_CHILD_LOGICAL);

	if ((zio->io_error ||
	    (zio->io_post & (ZIO_POST_REEXECUTE|ZIO_POST_SUSPEND))) &&
	    IO_IS_ALLOCATING(zio) && zio->io_gang_leader == zio &&
	    !(zio->io_flags & (ZIO_FLAG_IO_REWRITE | ZIO_FLAG_NOPWRITE)))
		zio_dva_unallocate(zio, zio->io_gang_tree, zio->io_bp);

	zio_gang_tree_free(&zio->io_gang_tree);

	/*
	 * Godfather I/Os should never suspend.
	 */
	if ((zio->io_flags & ZIO_FLAG_GODFATHER) &&
	    (zio->io_post & ZIO_POST_SUSPEND))
		zio->io_post &= ~ZIO_POST_SUSPEND;

	if (zio->io_post & (ZIO_POST_REEXECUTE|ZIO_POST_SUSPEND)) {
		/*
		 * A Direct I/O operation that has a checksum verify error
		 * should not attempt to reexecute. Instead, the error should
		 * just be propagated back.
		 */
		ASSERT0(zio->io_post & ZIO_POST_DIO_CHKSUM_ERR);

		/*
		 * This is a logical I/O that wants to reexecute.
		 *
		 * Reexecute is top-down.  When an i/o fails, if it's not
		 * the root, it simply notifies its parent and sticks around.
		 * The parent, seeing that it still has children in zio_done(),
		 * does the same.  This percolates all the way up to the root.
		 * The root i/o will reexecute or suspend the entire tree.
		 *
		 * This approach ensures that zio_reexecute() honors
		 * all the original i/o dependency relationships, e.g.
		 * parents not executing until children are ready.
		 */
		ASSERT(zio->io_child_type == ZIO_CHILD_LOGICAL);

		zio->io_gang_leader = NULL;

		mutex_enter(&zio->io_lock);
		zio->io_state[ZIO_WAIT_DONE] = 1;
		mutex_exit(&zio->io_lock);

		/*
		 * "The Godfather" I/O monitors its children but is
		 * not a true parent to them. It will track them through
		 * the pipeline but severs its ties whenever they get into
		 * trouble (e.g. suspended). This allows "The Godfather"
		 * I/O to return status without blocking.
		 */
		zl = NULL;
		for (pio = zio_walk_parents(zio, &zl); pio != NULL;
		    pio = pio_next) {
			zio_link_t *remove_zl = zl;
			pio_next = zio_walk_parents(zio, &zl);

			if ((pio->io_flags & ZIO_FLAG_GODFATHER) &&
			    (zio->io_post & ZIO_POST_SUSPEND)) {
				zio_remove_child(pio, zio, remove_zl);
				/*
				 * This is a rare code path, so we don't
				 * bother with "next_to_execute".
				 */
				zio_notify_parent(pio, zio, ZIO_WAIT_DONE,
				    NULL);
			}
		}

		if ((pio = zio_unique_parent(zio)) != NULL) {
			/*
			 * We're not a root i/o, so there's nothing to do
			 * but notify our parent.  Don't propagate errors
			 * upward since we haven't permanently failed yet.
			 */
			ASSERT(!(zio->io_flags & ZIO_FLAG_GODFATHER));
			zio->io_flags |= ZIO_FLAG_DONT_PROPAGATE;
			/*
			 * This is a rare code path, so we don't bother with
			 * "next_to_execute".
			 */
			zio_notify_parent(pio, zio, ZIO_WAIT_DONE, NULL);
		} else if (zio->io_post & ZIO_POST_SUSPEND) {
			/*
			 * We'd fail again if we reexecuted now, so suspend
			 * until conditions improve (e.g. device comes online).
			 */
			zio_suspend(zio->io_spa, zio, ZIO_SUSPEND_IOERR);
		} else {
			ASSERT(zio->io_post & ZIO_POST_REEXECUTE);
			/*
			 * Reexecution is potentially a huge amount of work.
			 * Hand it off to the otherwise-unused claim taskq.
			 */
			spa_taskq_dispatch(zio->io_spa,
			    ZIO_TYPE_CLAIM, ZIO_TASKQ_ISSUE,
			    zio_reexecute, zio, B_FALSE);
		}
		return (NULL);
	}

	ASSERT(list_is_empty(&zio->io_child_list));
	ASSERT0(zio->io_post & ZIO_POST_REEXECUTE);
	ASSERT0(zio->io_post & ZIO_POST_SUSPEND);
	ASSERT(zio->io_error == 0 || (zio->io_flags & ZIO_FLAG_CANFAIL));

	/*
	 * Report any checksum errors, since the I/O is complete.
	 */
	while (zio->io_cksum_report != NULL) {
		zio_cksum_report_t *zcr = zio->io_cksum_report;
		zio->io_cksum_report = zcr->zcr_next;
		zcr->zcr_next = NULL;
		zcr->zcr_finish(zcr, NULL);
		zfs_ereport_free_checksum(zcr);
	}

	/*
	 * It is the responsibility of the done callback to ensure that this
	 * particular zio is no longer discoverable for adoption, and as
	 * such, cannot acquire any new parents.
	 */
	if (zio->io_done)
		zio->io_done(zio);

	mutex_enter(&zio->io_lock);
	zio->io_state[ZIO_WAIT_DONE] = 1;
	mutex_exit(&zio->io_lock);

	/*
	 * We are done executing this zio.  We may want to execute a parent
	 * next.  See the comment in zio_notify_parent().
	 */
	zio_t *next_to_execute = NULL;
	zl = NULL;
	for (pio = zio_walk_parents(zio, &zl); pio != NULL; pio = pio_next) {
		zio_link_t *remove_zl = zl;
		pio_next = zio_walk_parents(zio, &zl);
		zio_remove_child(pio, zio, remove_zl);
		zio_notify_parent(pio, zio, ZIO_WAIT_DONE, &next_to_execute);
	}

	if (zio->io_waiter != NULL) {
		mutex_enter(&zio->io_lock);
		zio->io_executor = NULL;
		cv_broadcast(&zio->io_cv);
		mutex_exit(&zio->io_lock);
	} else {
		zio_destroy(zio);
	}

	return (next_to_execute);
}

/*
 * ==========================================================================
 * I/O pipeline definition
 * ==========================================================================
 */
static zio_pipe_stage_t *zio_pipeline[] = {
	NULL,
	zio_read_bp_init,
	zio_write_bp_init,
	zio_free_bp_init,
	zio_issue_async,
	zio_write_compress,
	zio_encrypt,
	zio_checksum_generate,
	zio_nop_write,
	zio_brt_free,
	zio_ddt_read_start,
	zio_ddt_read_done,
	zio_ddt_write,
	zio_ddt_free,
	zio_gang_assemble,
	zio_gang_issue,
	zio_dva_throttle,
	zio_dva_allocate,
	zio_dva_free,
	zio_dva_claim,
	zio_ready,
	zio_vdev_io_start,
	zio_vdev_io_done,
	zio_vdev_io_assess,
	zio_checksum_verify,
	zio_dio_checksum_verify,
	zio_done
};




/*
 * Compare two zbookmark_phys_t's to see which we would reach first in a
 * pre-order traversal of the object tree.
 *
 * This is simple in every case aside from the meta-dnode object. For all other
 * objects, we traverse them in order (object 1 before object 2, and so on).
 * However, all of these objects are traversed while traversing object 0, since
 * the data it points to is the list of objects.  Thus, we need to convert to a
 * canonical representation so we can compare meta-dnode bookmarks to
 * non-meta-dnode bookmarks.
 *
 * We do this by calculating "equivalents" for each field of the zbookmark.
 * zbookmarks outside of the meta-dnode use their own object and level, and
 * calculate the level 0 equivalent (the first L0 blkid that is contained in the
 * blocks this bookmark refers to) by multiplying their blkid by their span
 * (the number of L0 blocks contained within one block at their level).
 * zbookmarks inside the meta-dnode calculate their object equivalent
 * (which is L0equiv * dnodes per data block), use 0 for their L0equiv, and use
 * level + 1<<31 (any value larger than a level could ever be) for their level.
 * This causes them to always compare before a bookmark in their object
 * equivalent, compare appropriately to bookmarks in other objects, and to
 * compare appropriately to other bookmarks in the meta-dnode.
 */
int
zbookmark_compare(uint16_t dbss1, uint8_t ibs1, uint16_t dbss2, uint8_t ibs2,
    const zbookmark_phys_t *zb1, const zbookmark_phys_t *zb2)
{
	/*
	 * These variables represent the "equivalent" values for the zbookmark,
	 * after converting zbookmarks inside the meta dnode to their
	 * normal-object equivalents.
	 */
	uint64_t zb1obj, zb2obj;
	uint64_t zb1L0, zb2L0;
	uint64_t zb1level, zb2level;

	if (zb1->zb_object == zb2->zb_object &&
	    zb1->zb_level == zb2->zb_level &&
	    zb1->zb_blkid == zb2->zb_blkid)
		return (0);

	IMPLY(zb1->zb_level > 0, ibs1 >= SPA_MINBLOCKSHIFT);
	IMPLY(zb2->zb_level > 0, ibs2 >= SPA_MINBLOCKSHIFT);

	/*
	 * BP_SPANB calculates the span in blocks.
	 */
	zb1L0 = (zb1->zb_blkid) * BP_SPANB(ibs1, zb1->zb_level);
	zb2L0 = (zb2->zb_blkid) * BP_SPANB(ibs2, zb2->zb_level);

	if (zb1->zb_object == DMU_META_DNODE_OBJECT) {
		zb1obj = zb1L0 * (dbss1 << (SPA_MINBLOCKSHIFT - DNODE_SHIFT));
		zb1L0 = 0;
		zb1level = zb1->zb_level + COMPARE_META_LEVEL;
	} else {
		zb1obj = zb1->zb_object;
		zb1level = zb1->zb_level;
	}

	if (zb2->zb_object == DMU_META_DNODE_OBJECT) {
		zb2obj = zb2L0 * (dbss2 << (SPA_MINBLOCKSHIFT - DNODE_SHIFT));
		zb2L0 = 0;
		zb2level = zb2->zb_level + COMPARE_META_LEVEL;
	} else {
		zb2obj = zb2->zb_object;
		zb2level = zb2->zb_level;
	}

	/* Now that we have a canonical representation, do the comparison. */
	if (zb1obj != zb2obj)
		return (zb1obj < zb2obj ? -1 : 1);
	else if (zb1L0 != zb2L0)
		return (zb1L0 < zb2L0 ? -1 : 1);
	else if (zb1level != zb2level)
		return (zb1level > zb2level ? -1 : 1);
	/*
	 * This can (theoretically) happen if the bookmarks have the same object
	 * and level, but different blkids, if the block sizes are not the same.
	 * There is presently no way to change the indirect block sizes
	 */
	return (0);
}

/*
 *  This function checks the following: given that last_block is the place that
 *  our traversal stopped last time, does that guarantee that we've visited
 *  every node under subtree_root?  Therefore, we can't just use the raw output
 *  of zbookmark_compare.  We have to pass in a modified version of
 *  subtree_root; by incrementing the block id, and then checking whether
 *  last_block is before or equal to that, we can tell whether or not having
 *  visited last_block implies that all of subtree_root's children have been
 *  visited.
 */
boolean_t
zbookmark_subtree_completed(const dnode_phys_t *dnp,
    const zbookmark_phys_t *subtree_root, const zbookmark_phys_t *last_block)
{
	zbookmark_phys_t mod_zb = *subtree_root;
	mod_zb.zb_blkid++;
	ASSERT0(last_block->zb_level);

	/* The objset_phys_t isn't before anything. */
	if (dnp == NULL)
		return (B_FALSE);

	/*
	 * We pass in 1ULL << (DNODE_BLOCK_SHIFT - SPA_MINBLOCKSHIFT) for the
	 * data block size in sectors, because that variable is only used if
	 * the bookmark refers to a block in the meta-dnode.  Since we don't
	 * know without examining it what object it refers to, and there's no
	 * harm in passing in this value in other cases, we always pass it in.
	 *
	 * We pass in 0 for the indirect block size shift because zb2 must be
	 * level 0.  The indirect block size is only used to calculate the span
	 * of the bookmark, but since the bookmark must be level 0, the span is
	 * always 1, so the math works out.
	 *
	 * If you make changes to how the zbookmark_compare code works, be sure
	 * to make sure that this code still works afterwards.
	 */
	return (zbookmark_compare(dnp->dn_datablkszsec, dnp->dn_indblkshift,
	    1ULL << (DNODE_BLOCK_SHIFT - SPA_MINBLOCKSHIFT), 0, &mod_zb,
	    last_block) <= 0);
}

/*
 * This function is similar to zbookmark_subtree_completed(), but returns true
 * if subtree_root is equal or ahead of last_block, i.e. still to be done.
 */
boolean_t
zbookmark_subtree_tbd(const dnode_phys_t *dnp,
    const zbookmark_phys_t *subtree_root, const zbookmark_phys_t *last_block)
{
	ASSERT0(last_block->zb_level);
	if (dnp == NULL)
		return (B_FALSE);
	return (zbookmark_compare(dnp->dn_datablkszsec, dnp->dn_indblkshift,
	    1ULL << (DNODE_BLOCK_SHIFT - SPA_MINBLOCKSHIFT), 0, subtree_root,
	    last_block) >= 0);
}

EXPORT_SYMBOL(zio_type_name);
EXPORT_SYMBOL(zio_buf_alloc);
EXPORT_SYMBOL(zio_data_buf_alloc);
EXPORT_SYMBOL(zio_buf_free);
EXPORT_SYMBOL(zio_data_buf_free);

ZFS_MODULE_PARAM(zfs_zio, zio_, slow_io_ms, INT, ZMOD_RW,
	"Max I/O completion time (milliseconds) before marking it as slow");

ZFS_MODULE_PARAM(zfs_zio, zio_, requeue_io_start_cut_in_line, INT, ZMOD_RW,
	"Prioritize requeued I/O");

ZFS_MODULE_PARAM(zfs, zfs_, sync_pass_deferred_free,  UINT, ZMOD_RW,
	"Defer frees starting in this pass");

ZFS_MODULE_PARAM(zfs, zfs_, sync_pass_dont_compress, UINT, ZMOD_RW,
	"Don't compress starting in this pass");

ZFS_MODULE_PARAM(zfs, zfs_, sync_pass_rewrite, UINT, ZMOD_RW,
	"Rewrite new bps starting in this pass");

ZFS_MODULE_PARAM(zfs_zio, zio_, dva_throttle_enabled, INT, ZMOD_RW,
	"Throttle block allocations in the ZIO pipeline");

ZFS_MODULE_PARAM(zfs_zio, zio_, deadman_log_all, INT, ZMOD_RW,
	"Log all slow ZIOs, not just those with vdevs");
