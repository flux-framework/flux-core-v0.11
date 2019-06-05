/************************************************************\
 * Copyright 2019 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <inttypes.h>
#include <czmq.h>
#include <flux/core.h>
#include <flux/optparse.h>

#include "src/common/libsubprocess/subprocess.h"
#include "src/common/libutil/xzmalloc.h"
#include "src/common/libutil/log.h"
#include "src/bindings/lua/lua-hostlist/hostlist.h"

#ifndef VOLATILE
# if defined(__STDC__) || defined(__cplusplus)
# define VOLATILE volatile
# else
# define VOLATILE
# endif
#endif

#define MPIR_NULL                  0
#define MPIR_DEBUG_SPAWNED         1
#define MPIR_DEBUG_ABORTING        2

#define DEBUG(fmt,...) do { \
    if (verbose) log_msg(fmt, ##__VA_ARGS__); \
} while (0)

struct state_watch_ctx {
    flux_t *h;
    int64_t jobid;
    char *job_kvspath;
    hostlist_t hostlist;
    int pf;
    bool good_state;
};

typedef struct {
    char *host_name;
    char *executable_name;
    int pid;
} MPIR_PROCDESC;

VOLATILE int MPIR_being_debugged = 0;
VOLATILE int MPIR_debug_state    = MPIR_NULL;
MPIR_PROCDESC *MPIR_proctable    = NULL;
char *MPIR_debug_abort_string    = NULL;
char *totalview_jobid            = NULL;
int MPIR_proctable_size          = 0;
int MPIR_i_am_starter            = 1;
int MPIR_acquired_pre_main       = 1;
int MPIR_force_to_main           = 1;
int MPIR_partial_attach_ok       = 1;
int verbose                      = 0;

typedef enum { ATTACH_MODE = 0, LAUNCH_MODE } tool_mode_t;

static struct optparse_option cmdopts[] = {
    { .usage = "Command that interfaces with "
               "tools like parallel debuggers through MPIR.\n"
    },
    { .name = "attach", .key = 'a', .has_arg = 0,
      .usage = "Attach to Jobid (positional argument)."},
    { .name = "emulate", .key = 'e', .has_arg = 0,
      .usage = "Emulate as if it runs under tool."},
    { .name = "fanout", .key = 'f', .has_arg = 1, .flags = OPTPARSE_OPT_HIDDEN,
      .usage = "Limit the number of pending flux futures (default=32)."},
    { .name = "verbose", .key = 'v', .has_arg = 0,
      .usage = "Be verbose." },
    OPTPARSE_TABLE_END
};

extern char **environ;
extern void MPIR_Breakpoint ();


static char **build_newargv (size_t s, char **argv)
{
    int i = 0;
    char **newargv = NULL;
    if (s < 1 || argv == NULL)
        return NULL;
    newargv = (char **)xzmalloc ((s + 3) * sizeof (newargv));
    newargv[0] = strdup ("flux");
    newargv[1] = strdup (argv[0]);
    newargv[2] = strdup ("--options=stop-children-in-exec");
    for (i = 1; i < s; ++i)
        newargv[i+2] = strdup (argv[i]);
    newargv[i+2] = NULL;
    return newargv;
}

static void free_newargv (char **newargv)
{
    char **trav = newargv;
    while (*trav) {
        free (*trav);
        trav++;
    }
    free (newargv);
}

static void state_watch_ctx_init (struct state_watch_ctx **w, flux_t *h,
                                  int64_t jobid, const char *path,
                                  hostlist_t hl, int pf)
{
    (*w) = xzmalloc (sizeof (**w));
    (*w)->h = h;
    (*w)->jobid = jobid;
    (*w)->job_kvspath = strdup (path);
    (*w)->hostlist = hl;
    (*w)->pf = pf;
    (*w)->good_state = false;
}

static void state_watch_ctx_destroy (struct state_watch_ctx **w)
{
    free ((*w)->job_kvspath);
    hostlist_destroy ((*w)->hostlist);
    free (*w);
    *w = NULL;
}

static void completion_cb (flux_subprocess_t *p)
{
    flux_t *h;
    if (!(h = flux_subprocess_aux_get (p, "handle")))
        log_err_exit ("flux_subprocess_aux_set");

    DEBUG ("completion_cb is called");

    MPIR_debug_state = MPIR_DEBUG_ABORTING;
    MPIR_Breakpoint ();
    flux_reactor_stop (flux_get_reactor (h));
}

static void output_cb (flux_subprocess_t *p, const char *stream)
{
    int rc, lenp;
    const char *ptr;
    FILE *fstream = !strcasecmp (stream, "STDERR") ? stderr : stdout;

    if (!(ptr = flux_subprocess_read_line (p, stream, &lenp)))
        log_err_exit ("flux_subprocess_read_line");
    if (!lenp
        && flux_subprocess_state (p) == FLUX_SUBPROCESS_EXITED) {
        if (!(ptr = flux_subprocess_read (p, stream, -1, &lenp)))
            log_err_exit ("flux_subprocess_read");
    }
    if (lenp) {
        do {
            rc = fwrite (ptr, lenp, 1, fstream);
        } while (rc == -1 && errno == EINTR); /* Debuggers often cause EINTR */
    }
}

static void gen_attach_signal (flux_t *h, int64_t jobid)
{
    flux_future_t *f;
    char *topic = NULL;

    if (asprintf (&topic, "wreck.%" PRId64 ".kill", jobid) < 0)
        log_err_exit ("asprintf");
    if (!(f = flux_event_publish_pack (h, topic, 0, "{s:I}", "signal", 18)))
        log_err_exit ("flux_event_publish_pack");
    if (flux_future_get (f, NULL) < 0)
        log_err_exit ("publish %s: %s", topic, flux_strerror (errno));
    flux_future_destroy (f);

    DEBUG ("flux_event_publish_pack: %s (18) succeeded.", topic);

    free (topic);
}

static void gen_proctable_signal (flux_t *h, int64_t jobid)
{
    flux_future_t *f;
    char *topic = NULL;

    if (asprintf (&topic, "wreck.%" PRId64 ".proctable", jobid) < 0)
        log_err_exit ("asprintf");
    if (!(f = flux_event_publish (h, topic, 0, NULL)))
        log_err_exit ("flux_event_publish_pack");
    if (flux_future_get (f, NULL) < 0)
        log_err_exit ("publish %s: %s", topic, flux_strerror (errno));
    flux_future_destroy (f);

    DEBUG ("flux_event_publish_pack: %s succeeded.", topic);

    free (topic);
}

static flux_future_t *lookup_task_proctable (flux_t *h,
                                             const char *path, int rank)
{
    char *key = NULL;
    flux_future_t *f;

    if (asprintf (&key, "%s.%d.procdesc", path, rank) < 0)
        log_err_exit ("asprintf for procdesc");
    if (!(f = flux_kvs_lookup (h, FLUX_KVS_WAITCREATE, key)))
        log_err_exit ("flux_kvs_lookup for procdesc");
    free (key);
    return f;
}

static void lookup_get_task_proctable (flux_future_t *f,
                                       char **e, int *pid, int *nid)
{
    char *command;
    if (flux_kvs_lookup_get_unpack (f, "{s:s s:i s:i}",
                                       "command", &command,
                                       "pid", pid,
                                       "nodeid", nid) < 0)
        log_err_exit ("flux_kvs_lookup_get_unpack for procdesc");
    *e = strdup (command);
}

static void free_mpir_proctable (MPIR_PROCDESC *proctable, size_t s)
{
    int i;
    if (!proctable)
        return;

    for (i = 0; i < s; i++) {
        free (proctable[i].executable_name);
        free (proctable[i].host_name);
    }
    free (proctable);
}

static void fill_proctable_future (flux_t *h, int rank,
                                   const char *path, zlist_t *flist)
{
    flux_future_t *f = lookup_task_proctable (h, path, rank);
    zlist_append (flist, (void *)f);
    zlist_freefn (flist, (void *)f,
                  (zlist_free_fn *)flux_future_destroy, false);
}

static hostlist_t get_hostlist (flux_t *h)
{
    char *hl_str;
    flux_future_t *f;

    if (!(f = flux_kvs_lookup (h, 0, "resource.hosts")))
        log_err_exit ("flux_kvs_lookup for resource.hosts");
    if (flux_kvs_lookup_get_unpack (f, "s", &hl_str) < 0)
        log_err_exit ("flux_kvs_lookup_get_unpack for hostlist");

    DEBUG ("resource.hosts (%s)", hl_str);
    return hostlist_create (hl_str);
}

static void fill_task_proctable (flux_t *h, flux_future_t *f,
                                 int rank, hostlist_t hl, MPIR_PROCDESC *entry)
{
    char *en;
    char *hn;
    int pid, nid;

    lookup_get_task_proctable (f, &en, &pid, &nid);
    DEBUG ("Rank (%d): exec (%s), pid (%d), nodeid (%d)", rank, en, pid, nid);
    entry->executable_name = en; /* will be freed when entry is freed */
    if (!(hn = hostlist_nth (hl, nid)))
        log_err_exit ("hostlist_nth");
    entry->host_name = strdup (hostlist_nth (hl, nid));
    entry->pid = pid;
}

static void fill_mpir_proctable (flux_t *h, const char *path,
                                 hostlist_t hl, int pf)
{
    int i, b;
    zlist_t *flist;
    flux_future_t *trav;

    if (!MPIR_proctable)
        free_mpir_proctable (MPIR_proctable, MPIR_proctable_size);
    if (!(flist = zlist_new ()))
        log_err_exit ("zlist_new");

    MPIR_proctable = (MPIR_PROCDESC *)xzmalloc (MPIR_proctable_size
                                                * sizeof (MPIR_PROCDESC));

    for (b = 0; b < MPIR_proctable_size/pf; b++) {
        for (i = 0; i < pf; i++)
            fill_proctable_future (h, b*pf + i, path, flist);
        i = b * pf;
        for (trav = (flux_future_t *)zlist_first (flist); trav;
            trav = (flux_future_t *)zlist_next (flist)) {
            fill_task_proctable (h, trav, i, hl, &MPIR_proctable[i]);
            i++;
        }
        zlist_purge (flist);
    }

    for (i = 0; i < MPIR_proctable_size % pf; i++)
        fill_proctable_future (h, b*pf + i, path, flist);
    i = b * pf;
    for (trav = (flux_future_t *)zlist_first (flist); trav;
         trav = (flux_future_t *)zlist_next (flist)) {
        fill_task_proctable (h, trav, i, hl, &MPIR_proctable[i]);
        i++;
    }
    zlist_destroy (&flist);
}

static void fill_mpir (flux_t *h, int64_t jobid,
                       const char *path, hostlist_t hl, int pf)
{
    int64_t nnodes, ntasks, ncores, walltime, ngpus;

    jsc_query_rdesc_efficiently (h, jobid, &nnodes,
                                 &ntasks, &ncores, &walltime, &ngpus);

    MPIR_proctable_size = ntasks;

    if (totalview_jobid)
        free (totalview_jobid);
    if (asprintf (&totalview_jobid, "%"PRId64, jobid) < 0)
        log_err_exit ("asprintf");

    DEBUG ("nnodes (%d), ntasks (%d), ncores (%d), ngpus (%d)",
            (int)nnodes, (int)ntasks, (int)ncores, (int)ngpus);
    DEBUG ("totalview_jobid (%s)", totalview_jobid);
    DEBUG ("MPIR_proctable_size (%d)", (int)MPIR_proctable_size);

    fill_mpir_proctable (h, path, hl, pf);
}

static void handle_job_state (flux_future_t *f, void *arg)
{
    char *state;
    struct state_watch_ctx *w = (struct state_watch_ctx *)arg;

    if (flux_kvs_lookup_get_unpack (f, "s", &state) < 0) {
        if (errno == ENODATA) {
            state_watch_ctx_destroy (&w);
            flux_future_destroy (f);
            return;
        }
        else
            log_err_exit ("flux_kvs_lookup_get_unpack for looking up state");
    }
    flux_future_reset (f);

    DEBUG ("job state: %s", state);

    if (strncmp (state, "sync", 4) == 0
        || strncmp (state, "running", 7) == 0) {

        if (flux_kvs_lookup_cancel (f) < 0)
            log_err_exit ("flux_kvs_lookup_cancel");
        if (strncmp (state, "running", 7) == 0)
            gen_proctable_signal (w->h, w->jobid);
        fill_mpir (w->h, w->jobid, w->job_kvspath, w->hostlist, w->pf);
        w->good_state = true;
        MPIR_debug_state = MPIR_DEBUG_SPAWNED;
        MPIR_Breakpoint ();
        if (strncmp (state, "sync", 4) == 0)
            gen_attach_signal (w->h, w->jobid);
    } else if (strncmp (state, "reserved", 8) == 0
               || strncmp (state, "starting", 8) == 0) {
        // NOOP
    } else {
        errno = EINVAL;
        log_err_exit ("can't debug a job whose state=%s", state);
    }
}

static int setup_mpir_kvs (flux_t *h, int64_t jobid, const char *path,
                           hostlist_t hl, int pf)
{
    char *key;
    flux_future_t *f;
    struct state_watch_ctx *w;

    if (asprintf (&key, "%s%s", path, ".state") < 0)
        log_err_exit ("asprintf");
    state_watch_ctx_init (&w, h, jobid, path, hl, pf);
    if (!(f = flux_kvs_lookup (h, FLUX_KVS_WATCH, key)))
        log_err_exit ("flux_kvs_lookup");
    handle_job_state (f, w);
    if (!w->good_state)
        if (flux_future_then (f, -1.0, handle_job_state, (void *)w) < 0)
            log_err_exit ("flux_future_then");
    free (key);
    return w->good_state ? 0 : -1;
}

static int setup_mpir (flux_t *h, int64_t jobid, hostlist_t hl, int pf)
{
    flux_future_t *f;
    const char *path;
    if (!(f = flux_rpc_pack (h, "job.kvspath",
                             FLUX_NODEID_ANY, 0, "{s:[I]}", "ids", jobid))
        ||  flux_rpc_get_unpack (f, "{s:[s]}", "paths", &path) < 0)
        log_err_exit ("job.kvspath (nonexistent job?)");
    return setup_mpir_kvs (h, jobid, path, hl, pf);
}

static void setup_mpir_subprocess (flux_subprocess_t *p, int64_t jobid)
{
    int pf;
    flux_t *h;
    hostlist_t hl;
    DEBUG ("Jobid: %ld", (long)jobid);
    if (!(h = (flux_t *)flux_subprocess_aux_get (p, "handle")))
        log_err_exit ("flux_subprocess_aux_get (handle)");
    if (!(hl = (hostlist_t)flux_subprocess_aux_get (p, "hostlist")))
        log_err_exit ("flux_subprocess_aux_get (hostlist)");
    if (!(pf = (int)(intptr_t)flux_subprocess_aux_get (p, "fanout")))
        log_err_exit ("flux_subprocess_aux_get (fanout)");
    setup_mpir (h, jobid, hl, pf);
}

static void channel_cb (flux_subprocess_t *p, const char *stream)
{
    int rc, lenp;
    int64_t jobid;
    const char *ptr;

    if (!(ptr = flux_subprocess_read_line (p, stream, &lenp)))
        log_err_exit ("flux_subprocess_read_line");
    if (!lenp)
        return; /* EOF */
    do {
        rc = sscanf (ptr, "%"PRId64"\n", &jobid);
    } while (rc == -1 && errno == EINTR); /* Debuggers often cause EINTR */
    setup_mpir_subprocess (p, jobid);
}

static flux_subprocess_t *execute_job_run (flux_t *h, flux_cmd_t *cmd)
{
    flux_subprocess_ops_t ops = {
        .on_completion = completion_cb,
        .on_state_change = NULL,
        .on_channel_out = channel_cb,
        .on_stdout = output_cb,
        .on_stderr = output_cb
    };
    return flux_local_exec (flux_get_reactor (h), 0, cmd, &ops);
}

static void launch_with_tool (flux_t *h, flux_cmd_t *cmd, hostlist_t hl, int pf)
{
    flux_subprocess_t *p = NULL;
    if (!(p = execute_job_run (h, cmd)))
        log_err_exit ("execute_job_run");
    if (flux_subprocess_aux_set (p, "handle", (void *)h, NULL) < 0)
        log_err_exit ("flux_subprocess_aux_set (handle)");
    if (flux_subprocess_aux_set (p, "hostlist", (void *)hl, NULL) < 0)
        log_err_exit ("flux_subprocess_aux_set (hostlist)");
    if (flux_subprocess_aux_set (p, "fanout", (void *)(intptr_t)pf, NULL) < 0)
        log_err_exit ("flux_subprocess_aux_set (fanout)");
    if (flux_reactor_run (flux_get_reactor (h), 0) < 0)
        log_err_exit ("flux_reactor_run");
    flux_subprocess_destroy (p);

    DEBUG ("flux_reactor_run returned");
}

static void attach_with_tool (flux_t *h, int64_t jobid, hostlist_t hl, int pf)
{
    printf ("%d\n", (int)getpid ());

    while (!MPIR_being_debugged);

    gen_proctable_signal (h, jobid);
    if (setup_mpir (h, jobid, hl, pf) != 0) {
        if (flux_reactor_run (flux_get_reactor (h), 0) < 0)
            log_err_exit ("flux_reactor_run");
        DEBUG ("flux_reactor_run returned");
    }
}

void MPIR_Breakpoint ()
{
    DEBUG ("MPIR_Breakpoint is called");
    DEBUG ("Don't optimize this around!");
}

int main (int argc, char *argv[])
{
    int pf = 32;
    hostlist_t hl;
    int optindex = 0;
    flux_t *h = NULL;
    optparse_t *opts = NULL;
    flux_reactor_t *r = NULL;
    const char *optargp = NULL;
    tool_mode_t mode = LAUNCH_MODE;

    log_init ("flux-job-debug");

    opts = optparse_create ("flux-job-debug");
    if (optparse_add_option_table (opts, cmdopts) != OPTPARSE_SUCCESS)
        log_msg_exit ("optparse_add_option_table");
    if ((optindex = optparse_parse_args (opts, argc, argv)) < 0)
        log_msg_exit ("optparse_parse_args");
    if (optindex == argc) {
        optparse_print_usage (opts);
        exit (1);
    }
    if (optparse_getopt (opts, "attach", NULL) > 0)
        mode = ATTACH_MODE;
    if (optparse_getopt (opts, "emulate", NULL) > 0)
        MPIR_being_debugged = 1;
    if (optparse_getopt (opts, "fanout", &optargp) > 0)
        pf = atoi (optargp);
    if (optparse_getopt (opts, "verbose", NULL) > 0)
        verbose = 1;

    if (!(h = flux_open (NULL, 0)))
        log_err_exit ("flux_open");
    if (!(r = flux_reactor_create (FLUX_REACTOR_SIGCHLD)))
        log_err_exit ("flux_reactor_create");
    if (flux_set_reactor (h, r) < 0)
        log_err_exit ("flux_set_reactor");
    if (!(hl = get_hostlist (h)))
        log_err_exit ("hostlist");

    switch (mode) {
    case ATTACH_MODE: {
        char *remain = NULL;
        int64_t jobid = (int64_t) strtol (argv[optindex], &remain, 10);
        if (remain && *remain != '\0') {
            errno = EINVAL;
            log_err_exit ("Invalid jobid");
        }
        attach_with_tool (h, jobid, hl, pf);
        break;
    }
    case LAUNCH_MODE: {
        char **newargv = NULL;
        flux_cmd_t *cmd = NULL;
        if (!MPIR_being_debugged) {
            errno = EINVAL;
            log_err_exit ("MPIR_being_debugged != 0");
        }
        if (!(newargv = build_newargv (argc - optindex, &argv[optindex])))
            log_err_exit ("build_newargv");
        if (!(cmd = flux_cmd_create ((argc - optindex + 2), newargv, environ)))
            log_err_exit ("flux_cmd_create");
        if (flux_cmd_add_channel (cmd, "FLUX_WRECKRUN_JOBID_FD") < 0)
            log_err_exit ("flux_cmd_add_channel");
        launch_with_tool (h, cmd, hl, pf);
        free_newargv (newargv);
        flux_cmd_destroy (cmd);
        break;
    }
    default:
        errno = EINVAL;
        log_err_exit ("Unknown tool mode");
        break;
    }

    if (totalview_jobid)
        free (totalview_jobid);
    if (MPIR_proctable)
        free_mpir_proctable (MPIR_proctable, MPIR_proctable_size);

    flux_close (h);
    optparse_destroy (opts);
    log_fini ();

    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
