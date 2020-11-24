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
. $STF_SUITE/tests/functional/direct/dio.cfg
. $STF_SUITE/tests/functional/direct/dio.kshlib

#
# DESCRIPTION:
# 	Verify the number direct/buffered requests for (un)aligned access
#
# STRATEGY:
#	1. Create a multi-block file
#	2. Perform various (un)aligned accesses and verify the result.
#

verify_runnable "global"

if is_freebsd; then
	log_unsupported "Requires /proc/spl/kstat/zfs/*/objset-* file"
fi

function cleanup
{
	zfs set recordsize=$rs $TESTPOOL/$TESTFS
	log_must rm -f $tmp_file
}

log_onexit cleanup

log_assert "Verify the number direct/buffered requests for unaligned access"

mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
objset=$(get_objset_id $TESTPOOL/$TESTFS)

log_note "objset $TESTPOOL/$TESTFS objset ID $objset"

rs=$(get_prop recordsize $TESTPOOL/$TESTFS)
log_must zfs set recordsize=128k $TESTPOOL/$TESTFS

tmp_file=$mntpnt/tmp_file
file_size=$((rs * 8))

dd if=/dev/urandom of=$tmp_file bs=$file_size count=1

# N recordsize aligned writes which do not span blocks
check_write $TESTPOOL $objset $tmp_file $rs 1 0 direct 0 1
check_write $TESTPOOL $objset $tmp_file $rs 2 0 direct 0 2
check_write $TESTPOOL $objset $tmp_file $rs 4 0 direct 0 4
check_write $TESTPOOL $objset $tmp_file $rs 8 0 direct 0 8

# 1 recordsize aligned write which spans multiple blocks at various offsets
check_write $TESTPOOL $objset $tmp_file $((rs * 2)) 1 0 direct 0 1
check_write $TESTPOOL $objset $tmp_file $((rs * 2)) 1 1 direct 0 1
check_write $TESTPOOL $objset $tmp_file $((rs * 2)) 1 2 direct 0 1
check_write $TESTPOOL $objset $tmp_file $((rs * 2)) 1 3 direct 0 1
check_write $TESTPOOL $objset $tmp_file $((rs * 4)) 1 0 direct 0 1
check_write $TESTPOOL $objset $tmp_file $((rs * 4)) 1 1 direct 0 1
check_write $TESTPOOL $objset $tmp_file $((rs * 8)) 1 0 direct 0 1

# sub-blocksize unaligned writes which do not span blocks.
check_write $TESTPOOL $objset $tmp_file $((rs / 2)) 1 0 direct 1 0
check_write $TESTPOOL $objset $tmp_file $((rs / 2)) 1 1 direct 1 0
check_write $TESTPOOL $objset $tmp_file $((rs / 2)) 1 2 direct 1 0
check_write $TESTPOOL $objset $tmp_file $((rs / 2)) 1 3 direct 1 0

# large unaligned writes which span multiple blocks
check_write $TESTPOOL $objset $tmp_file $((rs * 2)) 1 $((rs / 2)) \
    direct,seek_bytes 1 1
check_write $TESTPOOL $objset $tmp_file $((rs * 4)) 2 $((rs / 4)) \
    direct,seek_bytes 2 2

# recordsize aligned reads which do not span blocks
check_read $TESTPOOL $objset $tmp_file $rs 1 0 direct 0 1
check_read $TESTPOOL $objset $tmp_file $rs 2 0 direct 0 2
check_read $TESTPOOL $objset $tmp_file $rs 4 0 direct 0 4
check_read $TESTPOOL $objset $tmp_file $rs 8 0 direct 0 8

# 1 recordsize aligned read which spans multiple blocks at various offsets
check_read $TESTPOOL $objset $tmp_file $((rs * 2)) 1 0 direct 0 1
check_read $TESTPOOL $objset $tmp_file $((rs * 2)) 1 1 direct 0 1
check_read $TESTPOOL $objset $tmp_file $((rs * 2)) 1 2 direct 0 1
check_read $TESTPOOL $objset $tmp_file $((rs * 2)) 1 3 direct 0 1
check_read $TESTPOOL $objset $tmp_file $((rs * 4)) 1 0 direct 0 1
check_read $TESTPOOL $objset $tmp_file $((rs * 4)) 1 1 direct 0 1
check_read $TESTPOOL $objset $tmp_file $((rs * 8)) 1 0 direct 0 1

# sub-blocksize unaligned reads which do not span blocks.
check_read $TESTPOOL $objset $tmp_file $((rs / 2)) 1 0 direct 0 1
check_read $TESTPOOL $objset $tmp_file $((rs / 2)) 1 1 direct 0 1
check_read $TESTPOOL $objset $tmp_file $((rs / 2)) 1 2 direct 0 1
check_read $TESTPOOL $objset $tmp_file $((rs / 2)) 1 3 direct 0 1

# large unaligned reads which span multiple blocks
check_read $TESTPOOL $objset $tmp_file $((rs * 2)) 1 $((rs / 2)) \
    direct,skip_bytes 0 1
check_read $TESTPOOL $objset $tmp_file $((rs * 4)) 2 $((rs / 4)) \
    direct,skip_bytes 0 2

log_pass "Verify the number direct/buffered requests for (un)aligned access"
