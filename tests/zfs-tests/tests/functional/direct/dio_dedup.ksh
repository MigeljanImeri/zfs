#!/bin/ksh -p
#
# DDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2021 by Lawrence Livermore National Security, LLC.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/include/properties.shlib
. $STF_SUITE/tests/functional/direct/dio.cfg
. $STF_SUITE/tests/functional/direct/dio.kshlib

#
# DESCRIPTION:
# 	Verify deduplication works using Direct IO.
#
# STRATEGY:
#	1. Enable dedup
#	2. Start sequential direct IO and verify with buffered IO
#	3. Start mixed direct IO and verify with buffered IO
#

verify_runnable "global"

function cleanup
{
	log_must rm -f "$mntpnt/direct-*"
	log_must zfs set dedup=off $TESTPOOL/$TESTFS
}

log_assert "Verify deduplication works using Direct IO."

log_onexit cleanup

mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
dedup_args="--dedupe_percentage=50"

log_must zfs set dedup=on $TESTPOOL/$TESTFS
dio_and_verify rw $DIO_FILESIZE $DIO_BS $mntpnt "sync" $dedup_args
dio_and_verify randrw $DIO_FILESIZE $DIO_BS $mntpnt "sync" $dedup_args

log_pass "Verfied deduplication works using Direct IO"
