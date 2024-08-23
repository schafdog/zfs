#!/usr/bin/env ksh -p
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
# Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2012, 2015 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#
# Valid datasets are accepted as bootfs property values
#
# STRATEGY:
# 1. Create a set of datasets in a test pool
# 2. Try setting them as boot filesystems
#

verify_runnable "global"

function cleanup {
	destroy_pool $TESTPOOL
	if [[ -f $VDEV ]]; then
		log_must $RM -f $VDEV
	fi
}

$ZPOOL set 2>&1 | $GREP bootfs > /dev/null
if [ $? -ne 0 ]
then
        log_unsupported "bootfs pool property not supported on this release."
fi

log_assert "Valid datasets are accepted as bootfs property values"
log_onexit cleanup

typeset VDEV=$TESTDIR/bootfs_001_pos_a.$$.dat

if [[ -n "$OSX" ]]; then
    log_must $MKFILE $MKFILE_SPARSE 400m $VDEV
else
    log_must $MKFILE -s 400m $VDEV
fi

create_pool "$TESTPOOL" "$VDEV"
log_must $ZFS create $TESTPOOL/$TESTFS

log_must $ZFS snapshot $TESTPOOL/$TESTFS@snap
log_must $ZFS clone $TESTPOOL/$TESTFS@snap $TESTPOOL/clone

log_must $ZPOOL set bootfs=$TESTPOOL/$TESTFS $TESTPOOL
log_must $ZPOOL set bootfs=$TESTPOOL/clone $TESTPOOL

log_must $ZFS promote $TESTPOOL/clone
log_must $ZPOOL set bootfs=$TESTPOOL/clone $TESTPOOL
log_pass "Valid datasets are accepted as bootfs property values"
