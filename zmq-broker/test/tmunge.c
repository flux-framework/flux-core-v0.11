/* tmunge.c - test MUNGE wrapper */

#include <sys/types.h>
#include <sys/time.h>
#include <json/json.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <string.h>
#include <zmq.h>
#include <czmq.h>
#include <libgen.h>
#include <pthread.h>

#include "flux.h"
#include "util.h"
#include "log.h"
#include "zmsg.h"
#include "security.h"

#if CZMQ_VERSION_MAJOR < 2
#define zmsg_pushstrf zmsg_pushstr
#endif

static const char *uri = "inproc://tmunge";
static int nframes;
static zctx_t *zctx;

void *thread (void *arg)
{
    void *zs;
    zmsg_t *zmsg;
    flux_sec_t sec;
    int i;

    if (!(sec = flux_sec_create ()))
        err_exit ("C: flux_sec_create");
    if (flux_sec_disable (sec, FLUX_SEC_TYPE_ALL) < 0)
        err_exit ("C: flux_sec_disable ALL");
    if (flux_sec_enable (sec, FLUX_SEC_TYPE_MUNGE) < 0)
        err_exit ("C: flux_sec_enable MUNGE");
    if (flux_sec_munge_init (sec) < 0)
        err_exit ("C: flux_sec_munge_init: %s", flux_sec_errstr (sec));

    if (!(zs = zsocket_new (zctx, ZMQ_PUB)))
        err_exit ("C: zsocket_new");

    if (zsocket_connect (zs, uri) < 0)
        err_exit ("C: zsocket_connect");

    if (!(zmsg = zmsg_new ()))
        oom ();
    for (i = nframes - 1; i >= 0; i--)
        if (zmsg_pushstrf (zmsg, "frame.%d", i) < 0)
            oom ();
    //zmsg_dump (zmsg);
    if (flux_sec_munge_zmsg (sec, &zmsg) < 0)
        err_exit ("C: flux_sec_munge_zmsg: %s", flux_sec_errstr (sec));
    //zmsg_dump (zmsg);
    if (zmsg_send (&zmsg, zs) < 0)
        err_exit ("C: zmsg_send");

    flux_sec_destroy (sec);

    return NULL;
}

int main (int argc, char *argv[])
{
    int rc;
    void *zs;
    pthread_t tid;
    pthread_attr_t attr;
    zmsg_t *zmsg;
    flux_sec_t sec;
    int n;

    log_init (basename (argv[0]));

    if (argc != 2) {
        fprintf (stderr, "Usage: tmunge nframes\n");
        exit (1);
    }
    nframes = strtoul (argv[1], NULL, 10);

    if (!(sec = flux_sec_create ()))
        err_exit ("flux_sec_create");
    if (flux_sec_disable (sec, FLUX_SEC_TYPE_ALL) < 0)
        err_exit ("flux_sec_disable ALL");
    if (flux_sec_enable (sec, FLUX_SEC_TYPE_MUNGE) < 0)
        err_exit ("flux_sec_enable MUNGE");
    if (flux_sec_munge_init (sec) < 0)
        err_exit ("flux_sec_munge_init: %s", flux_sec_errstr (sec));

    if (!(zctx = zctx_new ()))
        err_exit ("S: zctx_new");
    if (!(zs = zsocket_new (zctx, ZMQ_SUB)))
        err_exit ("S: zsocket_new");
    if (zsocket_bind (zs, uri) < 0)
        err_exit ("S: zsocket_bind");
    zsocket_set_subscribe (zs, "");

    if ((rc = pthread_attr_init (&attr)))
        errn (rc, "S: pthread_attr_init");
    if ((rc = pthread_create (&tid, &attr, thread, NULL)))
        errn (rc, "S: pthread_create");

    /* Handle one client message.
     */
    if (!(zmsg = zmsg_recv (zs)))
        err_exit ("S: zmsg_recv");
    //zmsg_dump (zmsg);
    if (flux_sec_unmunge_zmsg (sec, &zmsg) < 0)
        err_exit ("S: flux_sec_unmunge_zmsg: %s", flux_sec_errstr (sec));
    //zmsg_dump (zmsg);
    if ((n = zmsg_size (zmsg) != nframes))
        msg_exit ("S: expected %d frames, got %d", nframes, n);

    /* Wait for thread to terminate, then clean up.
     */
    if ((rc = pthread_join (tid, NULL)))
        errn (rc, "S: pthread_join");
    zctx_destroy (&zctx); /* destroys sockets too */

    flux_sec_destroy (sec);

    log_fini ();

    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
