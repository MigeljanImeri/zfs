#!/bin/ksh -p
#
# CDDL HEADER START
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
# Copyright (c) 2022 by Triad National Security, LLC.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/include/properties.shlib
. $STF_SUITE/tests/functional/direct/dio.cfg
. $STF_SUITE/tests/functional/direct/dio.kshlib

#
# DESCRIPTION:
# 	Verify FIO async engines work using Direct IO.
#
# STRATEGY:
#	1. Select a FIO async ioengine
#	2. Start sequntial direct IO and verify with buffered IO
#	3. Start mixed direct IO and verify with buffered IO
#

verify_runnable "global"

function cleanup
{
	log_must rm -f "$mntpnt/direct-*"
}

log_assert "Verify FIO async ioengines work using Direct IO."

log_onexit cleanup

typeset -a async_ioengine_args=("--iodepth=4" "--iodepth=4 --thread")

mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
fio_async_ioengines="posixaio"

if is_linux; then
	fio_async_ioengines+=" libaio"
fi

for ioengine in $fio_async_ioengines; do
	for ioengine_args in "${async_ioengine_args[@]}"; do
		log_note "Checking direct IO with FIO async ioengine" \
		    " $ioengine with args $ioengine_args"
		dio_and_verify rw $DIO_FILESIZE $DIO_BS $mntpnt "$ioengine" \
		    "$ioengine_args"
		dio_and_verify randrw $DIO_FILESIZE $DIO_BS $mntpnt \
		    "$ioengine" "$ioengine_args"
	done
done

log_pass "Verfied FIO async ioengines work using Direct IO"
