-- Set environment specific to spectrum_mpi (derived from openmpi)
--

if wreck:getopt ("mpi") ~= "spectrum" then return end

local posix = require 'posix'

function prepend_path (env_var, path)
    local env = wreck.environ
    if env[env_var] == nil then
       suffix = ''
    else
       suffix = ':'..env[env_var]
    end
    env[env_var] = path..suffix
end

local function strip_env_by_prefix (env, prefix)
    --
    -- Have to call env:get() to translate env object to Lua table
    --  in order to use pairs() to iterate environment keys:
    --
    for k,v in pairs (env:get()) do
        if k:match("^"..prefix) then
            env[k] = nil
        end
    end
end

function rexecd_init ()
    local env = wreck.environ
    local f = wreck.flux
    local rundir = f:getattr ('broker.rundir')

    -- Clear all existing PMIX_ and OMPI_ values before setting our own
    strip_env_by_prefix (env, "PMIX_")
    strip_env_by_prefix (env, "OMPI_")

    -- Avoid shared memory segment name collisions
    -- when flux instance runs >1 broker per node.
    env['OMPI_MCA_orte_tmpdir_base'] = rundir

    -- Assumes the installation paths of Spectrum MPI on LLNL's Sierra
    env['OMPI_MCA_osc'] = "pt2pt"
    env['OMPI_MCA_pml'] = "yalla"
    env['OMPI_MCA_btl'] = "self"
    env['OMPI_MCA_coll_hcoll_enable'] = '0'

    -- Help find libcollectives.so
    prepend_path ('LD_LIBRARY_PATH', '/opt/ibm/spectrum_mpi/lib/pami_port')
    prepend_path ('LD_PRELOAD', '/opt/ibm/spectrum_mpi/lib/libpami_cudahook.so')
end

function rexecd_task_init ()
    -- Approximately `ulimit -Ss 10240`
    -- Used to silence IBM MCM warnings
    posix.setrlimit ("stack", 10485760)
end

-- vi: ts=4 sw=4 expandtab
