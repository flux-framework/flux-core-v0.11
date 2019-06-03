#!/bin/sh
#

test_description='Test basic wreck procdesc functionality

Test basic functionality of wreck job debugger support
'

. `dirname $0`/sharness.sh
SIZE=${FLUX_TEST_SIZE:-4}
test_under_flux ${SIZE} wreck

KVSWAIT="$SHARNESS_TEST_SRCDIR/scripts/kvs-watch-until.lua"

test_expect_success 'wreckrun procdesc: -o stop-children-in-exec works' '
	flux wreckrun -n 4 -w sync -o stop-children-in-exec echo hello >output &&
	flux kvs get $(flux wreck last-jobid -p).0.procdesc &&
	flux wreck kill -s 9 $(flux wreck last-jobid) &&
	$KVSWAIT -t 1 $(flux wreck last-jobid -p).state "v == \"complete\""
'
test_expect_success 'wreckrun procdesc: procdesc dumped on proctable event' '
	flux wreckrun -n 4 -w running sleep 300 >output &&
	flux event pub wreck.$(flux wreck last-jobid).proctable &&
	flux kvs get --waitcreate $(flux wreck last-jobid -p).0.procdesc &&
	flux wreck kill -s 9 $(flux wreck last-jobid) &&
	$KVSWAIT -t 1 $(flux wreck last-jobid -p).state "v == \"complete\""
'
test_done
