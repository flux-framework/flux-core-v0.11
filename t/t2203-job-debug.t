#!/bin/sh
#

test_description='Test basic job-debug functionality

Test basic functionality of flux-job-debug facility.
'

. `dirname $0`/sharness.sh
SIZE=${FLUX_TEST_SIZE:-4}
test_under_flux ${SIZE} full

test_expect_success 'job-debug: works in attach mode on state=running' '
        flux wreckrun --detach -n${SIZE} sleep infinity &&
        J=$(flux wreck last-jobid) &&
        P=$(flux wreck last-jobid -p) &&
        ${SHARNESS_TEST_SRCDIR}/scripts/kvs-watch-until.lua -vt 1 ${P}.state "v == \"running\"" &&
        flux job-debug -e --attach ${J} &&
        flux wreck kill ${J}
'

test_expect_success 'job-debug: attach works when state=sync' '
        flux wreckrun --detach -n${SIZE} --options=stop-children-in-exec hostname &&
        J=$(flux wreck last-jobid) &&
        P=$(flux wreck last-jobid -p) &&
        ${SHARNESS_TEST_SRCDIR}/scripts/kvs-watch-until.lua -vt 1 ${P}.state "v == \"sync\"" &&
        flux job-debug -e --attach ${J}
'

test_expect_success 'job-debug: works in attach mode when state may be reserved/starting' '
        flux wreckrun --detach -n${SIZE} sleep infinity &&
        J=$(flux wreck last-jobid) &&
        P=$(flux wreck last-jobid -p) &&
        flux job-debug -e --attach ${J} &&
        flux wreck kill ${J}
'

test_expect_success 'job-debug: attach fails for other states' '
        run_timeout 5 flux wreckrun --detach -n${SIZE} hostname &&
        J=$(flux wreck last-jobid) &&
        P=$(flux wreck last-jobid -p) &&
        ${SHARNESS_TEST_SRCDIR}/scripts/kvs-watch-until.lua -vt 1 ${P}.state "v == \"complete\"" &&
        test_must_fail flux job-debug -e --attach ${J}
'

test_expect_success 'job-debug: attach fails on an incorrect jobid' '
        test_must_fail flux job-debug -e --attach 9999
'

test_expect_success 'job-debug: launch works' '
        run_timeout 5 flux job-debug -e wreckrun -n${size} hostname
'

test_expect_success 'job-debug: launch works with -f 1' '
        run_timeout 5 flux job-debug -f 1 -e wreckrun -n${size} hostname
'

test_expect_success 'job-debug: launch fails when MPIR_being_debugged != 1' '
        test_must_fail flux job-debug wreckrun -n${size} hostname
'

test_expect_success 'job-debug: launch fails on an incorrect launch command' '
        test_must_fail flux job-debug badrun -n${size} hostname
'

test_expect_success 'job-debug: launch fails with no positional argument' '
        test_must_fail flux job-debug
'

test_done

