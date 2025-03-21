// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2007 Pawel Jakub Dawidek <pjd@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Copyright (c) 2020, 2022 by Delphix. All rights reserved.
 */

#include <sys/param.h>
#include <sys/vfs.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libutil.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>

#include <libshare.h>
#include "libshare_impl.h"
#include "nfs.h"

#define	_PATH_MOUNTDPID	"/var/run/mountd.pid"
#define	ZFS_EXPORTS_FILE	"/etc/zfs/exports"
#define	ZFS_EXPORTS_LOCK	ZFS_EXPORTS_FILE".lock"

/*
 * This function translates options to a format acceptable by exports(5), eg.
 *
 *	-ro -network=192.168.0.0 -mask=255.255.255.0 -maproot=0 \
 *	zfs.freebsd.org 69.147.83.54
 *
 * Accepted input formats:
 *
 *	ro,network=192.168.0.0,mask=255.255.255.0,maproot=0,zfs.freebsd.org
 *	ro network=192.168.0.0 mask=255.255.255.0 maproot=0 zfs.freebsd.org
 *	-ro,-network=192.168.0.0,-mask=255.255.255.0,-maproot=0,zfs.freebsd.org
 *	-ro -network=192.168.0.0 -mask=255.255.255.0 -maproot=0 \
 *	zfs.freebsd.org
 *
 * Recognized keywords:
 *
 *	ro, maproot, mapall, mask, network, sec, alldirs, public, webnfs,
 *	index, quiet
 */
static int
translate_opts(char *oldopts, FILE *out)
{
	static const char *const known_opts[] = { "ro", "maproot", "mapall",
	    "mask", "network", "sec", "alldirs", "public", "webnfs", "index",
	    "quiet" };
	char *newopts, *o, *s = NULL;
	unsigned int i;
	size_t len, newopts_len;
	int ret;

	/*
	 * Calculate the length needed for the worst case of a single
	 * character option:
	 * - Add one to strlen(oldopts) so that the trailing nul is counted
	 *   as a separator.
	 * - Multiply by 3/2 since the single character option plus separator
	 *   is expanded to 3 characters.
	 * - Add one for the trailing nul.  Needed for a single repetition of
	 *   the single character option and certain other cases.
	 */
	newopts_len = (strlen(oldopts) + 1) * 3 / 2 + 1;
	newopts = malloc(newopts_len);
	if (newopts == NULL)
		return (EOF);
	newopts[0] = '\0';
	s = oldopts;
	while ((o = strsep(&s, ", ")) != NULL) {
		if (o[0] == '-')
			o++;
		if (o[0] == '\0')
			continue;
		for (i = 0; i < ARRAY_SIZE(known_opts); ++i) {
			len = strlen(known_opts[i]);
			if (strncmp(known_opts[i], o, len) == 0 &&
			    (o[len] == '\0' || o[len] == '=')) {
				strlcat(newopts, "-", newopts_len);
				break;
			}
		}
		strlcat(newopts, o, newopts_len);
		strlcat(newopts, " ", newopts_len);
	}
	ret = fputs(newopts, out);
	free(newopts);
	return (ret);
}

static int
nfs_enable_share_impl(sa_share_impl_t impl_share, FILE *tmpfile)
{
	const char *shareopts = impl_share->sa_shareopts;
	if (strcmp(shareopts, "on") == 0)
		shareopts = "";

	boolean_t need_free, fnd_semi;
	char *mp, *lineopts, *exportopts, *s;
	size_t whitelen;
	int rc  = nfs_escape_mountpoint(impl_share->sa_mountpoint, &mp,
	    &need_free);
	if (rc != SA_OK)
		return (rc);

	lineopts = strdup(shareopts);
	if (lineopts == NULL)
		return (SA_SYSTEM_ERR);
	s = lineopts;
	fnd_semi = B_FALSE;
	while ((exportopts = strsep(&s, ";")) != NULL) {
		if (s != NULL)
			fnd_semi = B_TRUE;
		/* Ignore only whitespace between ';' separated option sets. */
		if (fnd_semi) {
			whitelen = strspn(exportopts, "\t ");
			if (exportopts[whitelen] == '\0')
				continue;
		}
		if (fputs(mp, tmpfile) == EOF ||
		    fputc('\t', tmpfile) == EOF ||
		    translate_opts(exportopts, tmpfile) == EOF ||
		    fputc('\n', tmpfile) == EOF) {
			fprintf(stderr, "failed to write to temporary file\n");
			rc = SA_SYSTEM_ERR;
			break;
		}
	}
	free(lineopts);

	if (need_free)
		free(mp);
	return (rc);
}

static int
nfs_enable_share(sa_share_impl_t impl_share)
{
	return (nfs_toggle_share(
	    ZFS_EXPORTS_LOCK, ZFS_EXPORTS_FILE, NULL, impl_share,
	    nfs_enable_share_impl));
}

static int
nfs_disable_share_impl(sa_share_impl_t impl_share, FILE *tmpfile)
{
	(void) impl_share, (void) tmpfile;
	return (SA_OK);
}

static int
nfs_disable_share(sa_share_impl_t impl_share)
{
	return (nfs_toggle_share(
	    ZFS_EXPORTS_LOCK, ZFS_EXPORTS_FILE, NULL, impl_share,
	    nfs_disable_share_impl));
}

static boolean_t
nfs_is_shared(sa_share_impl_t impl_share)
{
	return (nfs_is_shared_impl(ZFS_EXPORTS_FILE, impl_share));
}

static int
nfs_validate_shareopts(const char *shareopts)
{
	if (strlen(shareopts) == 0)
		return (SA_SYNTAX_ERR);
	return (SA_OK);
}

/*
 * Commit the shares by restarting mountd.
 */
static int
nfs_commit_shares(void)
{
	struct pidfh *pfh;
	pid_t mountdpid;

start:
	pfh = pidfile_open(_PATH_MOUNTDPID, 0600, &mountdpid);
	if (pfh != NULL) {
		/* mountd(8) is not running. */
		pidfile_remove(pfh);
		return (SA_OK);
	}
	if (errno != EEXIST) {
		/* Cannot open pidfile for some reason. */
		return (SA_SYSTEM_ERR);
	}
	if (mountdpid == -1) {
		/* mountd(8) exists, but didn't write the PID yet */
		usleep(500);
		goto start;
	}
	/* We have mountd(8) PID in mountdpid variable. */
	kill(mountdpid, SIGHUP);
	return (SA_OK);
}

static void
nfs_truncate_shares(void)
{
	nfs_reset_shares(ZFS_EXPORTS_LOCK, ZFS_EXPORTS_FILE);
}

const sa_fstype_t libshare_nfs_type = {
	.enable_share = nfs_enable_share,
	.disable_share = nfs_disable_share,
	.is_shared = nfs_is_shared,

	.validate_shareopts = nfs_validate_shareopts,
	.commit_shares = nfs_commit_shares,
	.truncate_shares = nfs_truncate_shares,
};
