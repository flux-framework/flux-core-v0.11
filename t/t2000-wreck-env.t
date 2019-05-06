#!/bin/sh
#

test_description='Test basic wreck functionality

Test basic functionality of wreckrun facility.
'

. `dirname $0`/sharness.sh
SIZE=${FLUX_TEST_SIZE:-4}
test_under_flux ${SIZE} wreck

#  Return the previous jobid
last_job_id() {
	flux wreck last-jobid
}
#  Return previous job path in kvs
last_job_path() {
	flux wreck last-jobid -p
}

test_expect_success 'flux-wreck: setenv/getenv works' '
	flux wreck setenv FOO=BAR &&
	flux wreck getenv FOO
'
test_expect_success 'flux-wreck: unsetenv works' '
	flux wreck unsetenv FOO &&
	test "$(flux wreck getenv FOO)" = "FOO="
'
test_expect_success 'flux-wreck: setenv all' '
	flux wreck setenv all &&
	flux env /usr/bin/env | sort | grep -ve ^FLUX_URI= -e ^HOSTNAME= -e ^ENVIRONMENT= > env.expected &&
	flux wreck getenv | sort > env.output &&
	test_cmp env.expected env.output
'
test_expect_success 'wreck: global lwj.environ exported to jobs' '
	flux wreck setenv FOO=bar &&
	test "$(flux wreckrun -n1 printenv FOO)" = "bar"
'
test_expect_success 'wreck: wreckrun exports environment vars not in global env' '
	BAR=baz flux wreckrun -n1 printenv BAR > printenv.out &&
	test "$(cat printenv.out)" = "baz"
'
test_expect_success 'wreck: wreckrun --skip-env works' '
	( export BAR=baz &&
          test_must_fail  flux wreckrun --skip-env -n1 printenv BAR > printenv2.out
	) &&
	test "$(cat printenv2.out)" = ""
'
test_expect_success 'wreck plugins can use wreck:environ:get()' '
	saved_pattern=$(flux getattr wrexec.lua_pattern) &&
	if test $? = 0; then
	  test_when_finished \
	    "flux setattr wrexec.lua_pattern \"$saved_pattern\""
	else
	  test_when_finished \
	     "flux setattr --expunge wrexec.lua_pattern"
	fi &&
	cat <<-EOF >test.lua &&
	function rexecd_init ()
	    local env = wreck.environ:get()
	    for k,v in pairs(env) do
	        if k:match ("UNSETME_") then wreck.environ [k] = nil end
	    end
	end
	EOF
	UNSETME_foo=1 flux wreckrun /usr/bin/env > all_env.out &&
	grep UNSETME all_env.out &&
	flux setattr wrexec.lua_pattern "$(pwd)/*.lua" &&
	UNSETME_foo=1 flux wreckrun /usr/bin/env > all_env.out &&
	test_expect_code 1 grep UNSETME all_env.out
'

test_done
