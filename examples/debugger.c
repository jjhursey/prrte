/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2018 Intel, Inc. All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include <pmix_tool.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool active;
    pmix_status_t status;
} mylock_t;

#define DEBUG_CONSTRUCT_LOCK(l)                     \
    do {                                            \
        pthread_mutex_init(&(l)->mutex, NULL);      \
        pthread_cond_init(&(l)->cond, NULL);        \
        (l)->active = true;                         \
        (l)->status = PMIX_SUCCESS;                 \
    } while(0)

#define DEBUG_DESTRUCT_LOCK(l)              \
    do {                                    \
        pthread_mutex_destroy(&(l)->mutex); \
        pthread_cond_destroy(&(l)->cond);   \
    } while(0)

#define DEBUG_WAIT_THREAD(lck)                                      \
    do {                                                            \
        pthread_mutex_lock(&(lck)->mutex);                          \
        while ((lck)->active) {                                     \
            pthread_cond_wait(&(lck)->cond, &(lck)->mutex);         \
        }                                                           \
        pthread_mutex_unlock(&(lck)->mutex);                        \
    } while(0)

#define DEBUG_WAKEUP_THREAD(lck)                        \
    do {                                                \
        pthread_mutex_lock(&(lck)->mutex);              \
        (lck)->active = false;                          \
        pthread_cond_broadcast(&(lck)->cond);           \
        pthread_mutex_unlock(&(lck)->mutex);            \
    } while(0)

/* define a structure for collecting returned
 * info from a query */
typedef struct {
    mylock_t lock;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
} myquery_data_t;

/* define a structure for releasing when a given
 * nspace terminates */
typedef struct {
    mylock_t lock;
    char *nspace;
    int exit_code;
    bool exit_code_given;
} myrel_t;


static int attach_to_running_job(char *nspace);
static pmix_proc_t myproc;

/* this is a callback function for the PMIx_Query
 * API. The query will callback with a status indicating
 * if the request could be fully satisfied, partially
 * satisfied, or completely failed. The info parameter
 * contains an array of the returned data, with the
 * info->key field being the key that was provided in
 * the query call. Thus, you can correlate the returned
 * data in the info->value field to the requested key.
 *
 * Once we have dealt with the returned data, we must
 * call the release_fn so that the PMIx library can
 * cleanup */
static void cbfunc(pmix_status_t status,
                   pmix_info_t *info, size_t ninfo,
                   void *cbdata,
                   pmix_release_cbfunc_t release_fn,
                   void *release_cbdata)
{
    myquery_data_t *mq = (myquery_data_t*)cbdata;
    size_t n;

    mq->status = status;
    /* save the returned info - the PMIx library "owns" it
     * and will release it and perform other cleanup actions
     * when release_fn is called */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(mq->info, ninfo);
        mq->ninfo = ninfo;
        for (n=0; n < ninfo; n++) {
            fprintf(stderr, "Key %s Type %s(%d)\n", info[n].key, PMIx_Data_type_string(info[n].value.type), info[n].value.type);
            PMIX_INFO_XFER(&mq->info[n], &info[n]);
        }
    }

    /* let the library release the data and cleanup from
     * the operation */
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }

    /* release the block */
    DEBUG_WAKEUP_THREAD(&mq->lock);
}

/* this is the event notification function we pass down below
 * when registering for general events - i.e.,, the default
 * handler. We don't technically need to register one, but it
 * is usually good practice to catch any events that occur */
static void notification_fn(size_t evhdlr_registration_id,
                            pmix_status_t status,
                            const pmix_proc_t *source,
                            pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc,
                            void *cbdata)
{
    /* this example doesn't do anything with default events */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/* this is an event notification function that we explicitly request
 * be called when the PMIX_ERR_JOB_TERMINATED notification is issued.
 * We could catch it in the general event notification function and test
 * the status to see if it was "job terminated", but it often is simpler
 * to declare a use-specific notification callback point. In this case,
 * we are asking to know whenever a job terminates, and we will then
 * know we can exit */
static void release_fn(size_t evhdlr_registration_id,
                       pmix_status_t status,
                       const pmix_proc_t *source,
                       pmix_info_t info[], size_t ninfo,
                       pmix_info_t results[], size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc,
                       void *cbdata)
{
    myrel_t *lock;
    pmix_status_t rc;
    bool found;
    int exit_code;
    size_t n;
    pmix_proc_t *affected = NULL;

    /* find our return object */
    lock = NULL;
    found = false;
    for (n=0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
            lock = (myrel_t*)info[n].value.data.ptr;
            /* not every RM will provide an exit code, but check if one was given */
        } else if (0 == strncmp(info[n].key, PMIX_EXIT_CODE, PMIX_MAX_KEYLEN)) {
            exit_code = info[n].value.data.integer;
            found = true;
        } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
            affected = info[n].value.data.proc;
        }
    }
    /* if the object wasn't returned, then that is an error */
    if (NULL == lock) {
        fprintf(stderr, "LOCK WASN'T RETURNED IN RELEASE CALLBACK\n");
        /* let the event handler progress */
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }

    /* see if the code is LAUNCHER_READY */
    if (PMIX_LAUNCHER_READY == status) {
            fprintf(stderr, "%d DEBUGGER NOTIFIED THAT LAUNCHER IS READY\n", (int)getpid());
    } else {
        fprintf(stderr, "DEBUGGER NOTIFIED THAT JOB %s TERMINATED - AFFECTED %s\n", lock->nspace,
                (NULL == affected) ? "NULL" : affected->nspace);
        if (found) {
            lock->exit_code = exit_code;
            lock->exit_code_given = true;
        }
    }
    DEBUG_WAKEUP_THREAD(&lock->lock);

    /* tell the event handler state machine that we are the last step */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    return;
}

/* event handler registration is done asynchronously because it
 * may involve the PMIx server registering with the host RM for
 * external events. So we provide a callback function that returns
 * the status of the request (success or an error), plus a numerical index
 * to the registered event. The index is used later on to deregister
 * an event handler - if we don't explicitly deregister it, then the
 * PMIx server will do so when it see us exit */
static void evhandler_reg_callbk(pmix_status_t status,
                                 size_t evhandler_ref,
                                 void *cbdata)
{
    mylock_t *lock = (mylock_t*)cbdata;

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                   myproc.nspace, myproc.rank, status, (unsigned long)evhandler_ref);
    }
    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

static pmix_status_t spawn_debugger(char *appspace, myrel_t *myrel)
{
    pmix_status_t rc;
    pmix_info_t *dinfo;
    pmix_app_t *debugger;
    size_t dninfo;
    char cwd[1024];
    char dspace[PMIX_MAX_NSLEN+1];
    mylock_t mylock;
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;

    /* setup the debugger */
    PMIX_APP_CREATE(debugger, 1);
    debugger[0].cmd = strdup("./debuggerd");
    PMIX_ARGV_APPEND(rc, debugger[0].argv, "./debuggerd");
    getcwd(cwd, 1024);  // point us to our current directory
    debugger[0].cwd = strdup(cwd);
    /* provide directives so the daemons go where we want, and
     * let the RM know these are debugger daemons */
    dninfo = 6;
    PMIX_INFO_CREATE(dinfo, dninfo);
    PMIX_INFO_LOAD(&dinfo[0], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);  // instruct the RM to launch one copy of the executable on each node
    PMIX_INFO_LOAD(&dinfo[1], PMIX_DEBUGGER_DAEMONS, NULL, PMIX_BOOL); // these are debugger daemons
    PMIX_INFO_LOAD(&dinfo[1], PMIX_DEBUG_JOB, appspace, PMIX_STRING); // the nspace being debugged
    PMIX_INFO_LOAD(&dinfo[2], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL); // notify us when the debugger job completes
    PMIX_INFO_LOAD(&dinfo[3], PMIX_DEBUG_WAITING_FOR_NOTIFY, NULL, PMIX_BOOL);  // tell the daemon that the proc is waiting to be released
    PMIX_INFO_LOAD(&dinfo[4], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);  // forward stdout to me
    PMIX_INFO_LOAD(&dinfo[5], PMIX_FWD_STDERR, NULL, PMIX_BOOL);  // forward stderr to me
    /* spawn the daemons */
    fprintf(stderr, "Debugger: spawning %s\n", debugger[0].cmd);
    if (PMIX_SUCCESS != (rc = PMIx_Spawn(dinfo, dninfo, debugger, 1, dspace))) {
        fprintf(stderr, "Debugger daemons failed to launch with error: %s\n", PMIx_Error_string(rc));
        PMIX_INFO_FREE(dinfo, dninfo);
        PMIX_APP_FREE(debugger, 1);
        return rc;
    }
    /* cleanup */
    PMIX_INFO_FREE(dinfo, dninfo);
    PMIX_APP_FREE(debugger, 1);

    /* register callback for when this job terminates */
    myrel->nspace = strdup(dspace);
    PMIX_INFO_CREATE(dinfo, 2);
    PMIX_INFO_LOAD(&dinfo[0], PMIX_EVENT_RETURN_OBJECT, myrel, PMIX_POINTER);
    /* only call me back when this specific job terminates */
    PMIX_INFO_LOAD(&dinfo[1], PMIX_NSPACE, dspace, PMIX_STRING);

    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, dinfo, 2,
                                release_fn, evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    rc = mylock.status;
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(dinfo, 2);

    return rc;
}

#define DBGR_LOOP_LIMIT  10

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t *info, *iptr;
    pmix_app_t *app;
    size_t ninfo, napps;
    char *nspace = NULL;
    int i;
    pmix_query_t *query;
    size_t nq, n;
    myquery_data_t myquery_data;
    bool cospawn = false, stop_on_exec = false, cospawn_reqd = false;
    char cwd[1024];
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;
    mylock_t mylock;
    myrel_t myrel, launcher_ready, dbrel;
    pid_t pid;
    pmix_envar_t envar;
    char *launchers[] = {
        "prun",
        "mpirun",
        "mpiexec",
        "orterun",
        NULL
    };
    pmix_proc_t proc;
    bool found;
    pmix_data_array_t darray;
    char *tmp;
    char clientspace[PMIX_MAX_NSLEN+1];

    pid = getpid();

    /* Process any arguments we were given */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "-h") ||
            0 == strcmp(argv[i], "--help")) {
            /* print the usage message and exit */

        }
        if (0 == strcmp(argv[i], "-a") ||
            0 == strcmp(argv[i], "--attach")) {
            if (NULL != nspace) {
                /* can only support one */
                fprintf(stderr, "Cannot attach to more than one nspace\n");
                exit(1);
            }
            /* the next argument must be the nspace */
            ++i;
            if (argc == i) {
                /* they goofed */
                fprintf(stderr, "The %s option requires an <nspace> argument\n", argv[i]);
                exit(1);
            }
            nspace = strdup(argv[i]);
        } else if (0 == strcmp(argv[i], "-c") ||
                   0 == strcmp(argv[i], "--cospawn")){
            cospawn_reqd = true;
            break;
        }
    }
    info = NULL;
    ninfo = 0;

    /* use the system connection first, if available */
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    /* init as a tool */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, ninfo))) {
        fprintf(stderr, "PMIx_tool_init failed: %s(%d)\n", PMIx_Error_string(rc), rc);
        exit(rc);
    }
    PMIX_INFO_FREE(info, ninfo);

    fprintf(stderr, "Debugger ns %s rank %d pid %lu: Running\n", myproc.nspace, myproc.rank, (unsigned long)pid);

    /* construct the debugger termination release */
    DEBUG_CONSTRUCT_LOCK(&dbrel.lock);

    /* register a default event handler */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0,
                                notification_fn, evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    DEBUG_DESTRUCT_LOCK(&mylock);

    /* if we are attaching to a running job, then attach to it */
    if (NULL != nspace) {
        if (PMIX_SUCCESS != (rc = attach_to_running_job(nspace))) {
            fprintf(stderr, "Failed to attach to nspace %s: error code %d\n",
                    nspace, rc);
            goto done;
        }
        goto rundebugger;
    }

    /* check to see if we are using an intermediate launcher - we only
     * support those we recognize */
    found = false;
    if (1 < argc) {
        for (n=0; NULL != launchers[n]; n++) {
            if (0 == strcmp(argv[1], launchers[n])) {
                found = true;
            }
        }
    }
    if (found) {
        /* register to receive the "launcher-ready" event telling us
         * that the launcher is ready for us to connect to it */
        DEBUG_CONSTRUCT_LOCK(&mylock);
        code = PMIX_LAUNCHER_READY;
        /* pass a lock object to release us when the launcher is ready */
        DEBUG_CONSTRUCT_LOCK(&launcher_ready.lock);
        PMIX_INFO_CREATE(info, 2);
        PMIX_INFO_LOAD(&info[0], PMIX_EVENT_RETURN_OBJECT, &launcher_ready, PMIX_POINTER);
        PMIX_INFO_LOAD(&info[1], PMIX_EVENT_HDLR_NAME, "LAUNCHER-READY", PMIX_STRING);
        PMIx_Register_event_handler(&code, 1, info, 2,
                                    release_fn, evhandler_reg_callbk, (void*)&mylock);
        DEBUG_WAIT_THREAD(&mylock);
        if (PMIX_SUCCESS != mylock.status) {
            rc = mylock.status;
            DEBUG_DESTRUCT_LOCK(&mylock);
            PMIX_INFO_FREE(info, 2);
            goto done;
        }
        DEBUG_DESTRUCT_LOCK(&mylock);
        PMIX_INFO_FREE(info, 2);

        /* we are using an intermediate launcher - we will use the
         * reference server to start it, but tell it to wait after
         * launch for directive prior to spawning the application */
        napps = 1;
        PMIX_APP_CREATE(app, napps);
        /* setup the executable */
        app[0].cmd = strdup(argv[1]);
        PMIX_ARGV_APPEND(rc, app[0].argv, argv[1]);
        for (n=2; n < argc; n++) {
            PMIX_ARGV_APPEND(rc, app[0].argv, argv[n]);
        }
        getcwd(cwd, 1024);  // point us to our current directory
        app[0].cwd = strdup(cwd);
        app[0].maxprocs = 1;
        /* provide job-level directives so the apps do what the user requested */
#ifdef PMIX_LAUNCHER_RENDEZVOUS_FILE
        ninfo = 6;
#else
        ninfo = 5;
#endif
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[0], PMIX_MAPBY, "slot", PMIX_STRING);  // map by slot
        asprintf(&tmp, "%s:%d", myproc.nspace, myproc.rank);
        PMIX_ENVAR_LOAD(&envar, "PMIX_LAUNCHER_PAUSE_FOR_TOOL", tmp, ':');
        free(tmp);
        PMIX_INFO_LOAD(&info[1], PMIX_SET_ENVAR, &envar, PMIX_ENVAR);  // launcher is to wait for directives
        PMIX_ENVAR_DESTRUCT(&envar);
        cospawn = false;
        PMIX_INFO_LOAD(&info[2], PMIX_FWD_STDOUT, &cospawn, PMIX_BOOL);  // forward stdout to me
        PMIX_INFO_LOAD(&info[3], PMIX_FWD_STDERR, &cospawn, PMIX_BOOL);  // forward stderr to me
        PMIX_INFO_LOAD(&info[4], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL); // notify us when the job completes
#ifdef PMIX_LAUNCHER_RENDEZVOUS_FILE
        PMIX_INFO_LOAD(&info[5], PMIX_LAUNCHER_RENDEZVOUS_FILE, "dbgr.rndz.txt", PMIX_STRING);  // have it output a specific rndz file
#endif
        /* spawn the job - the function will return when the launcher
         * has been launched. Note that this doesn't tell us anything
         * about the launcher's state - it just means that the launcher
         * has been fork/exec'd */
        fprintf(stderr, "Debugger: spawning %s\n", app[0].cmd);
        rc = PMIx_Spawn(info, ninfo, app, napps, clientspace);
        PMIX_INFO_FREE(info, ninfo);
        PMIX_APP_FREE(app, napps);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Application failed to launch with error: %s(%d)\n", PMIx_Error_string(rc), rc);
            goto done;
        }

        /* wait here for the launcher to declare itself ready */
        DEBUG_WAIT_THREAD(&launcher_ready.lock);
        DEBUG_DESTRUCT_LOCK(&launcher_ready.lock);


        /* transfer our connection to the spawned launcher - by making it our
         * server, we can query it for information about the job it launched */
        PMIX_INFO_CREATE(info, 2);
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_NSPACE, clientspace, PMIX_STRING);  // find rendezvous info by nspace
        PMIX_INFO_LOAD(&info[1], PMIX_RECONNECT_SERVER, NULL, PMIX_BOOL);  // we are reconnecting
        rc = PMIx_tool_connect_to_server(NULL, info, 2);
        PMIX_INFO_FREE(info, 2);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Failed to connect to %s server: %s(%d)\n", argv[1], PMIx_Error_string(rc), rc);
            goto done;
        }
fprintf(stderr, "Connection transferred to launcher\n");
        /* send the launch directives */
        ninfo = 3;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_PROC_LOAD(&proc, clientspace, 0);
        PMIX_INFO_LOAD(&info[0], PMIX_EVENT_CUSTOM_RANGE, &proc, PMIX_PROC);  // deliver to the target launcher
        PMIX_INFO_LOAD(&info[1], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);  // only non-default handlers
        /* provide a few job-level directives */
        darray.type = PMIX_INFO;
        darray.size = 2;
        PMIX_INFO_CREATE(darray.array, 2);
        iptr = (pmix_info_t*)darray.array;
        PMIX_ENVAR_LOAD(&envar, "FOOBAR", "1", ':');
        PMIX_INFO_LOAD(&iptr[0], PMIX_SET_ENVAR, &envar, PMIX_ENVAR);
        PMIX_ENVAR_DESTRUCT(&envar);
        PMIX_ENVAR_LOAD(&envar, "PATH", "/home/common/local/toad", ':');
        PMIX_INFO_LOAD(&iptr[1], PMIX_PREPEND_ENVAR, &envar, PMIX_ENVAR);
        PMIX_ENVAR_DESTRUCT(&envar);

        PMIX_INFO_LOAD(&info[2], PMIX_DEBUG_JOB_DIRECTIVES, &darray, PMIX_DATA_ARRAY);
        /* provide a few app-level directives */
        PMIX_INFO_LOAD(&info[3], PMIX_DEBUG_APP_DIRECTIVES, &darray, PMIX_DATA_ARRAY);

        fprintf(stderr, "[%s:%u%lu] Sending launch directives\n", myproc.nspace, myproc.rank, (unsigned long)pid);
        PMIx_Notify_event(PMIX_LAUNCH_DIRECTIVE,
                          NULL, PMIX_RANGE_LOCAL,
                          info, ninfo, NULL, NULL);
        PMIX_INFO_FREE(info, ninfo);
    } else {
        /* this is an initial launch - we need to launch the application
         * plus the debugger daemons, letting the RM know we are debugging
         * so that it will "pause" the app procs until we are ready. First
         * we need to know if this RM supports co-spawning of daemons with
         * the application, or if we need to launch the daemons as a separate
         * spawn command. The former is faster and more scalable, but not
         * every RM may support it. We also need to ask for debug support
         * so we know if the RM can stop-on-exec, or only supports stop-in-init */
        nq = 1;
        PMIX_QUERY_CREATE(query, nq);
        PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_SPAWN_SUPPORT);
        PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_DEBUG_SUPPORT);
        /* setup the caddy to retrieve the data */
        DEBUG_CONSTRUCT_LOCK(&myquery_data.lock);
        myquery_data.info = NULL;
        myquery_data.ninfo = 0;
        /* execute the query */
        if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(query, nq, cbfunc, (void*)&myquery_data))) {
            fprintf(stderr, "PMIx_Query_info failed: %d\n", rc);
            goto done;
        }
        DEBUG_WAIT_THREAD(&myquery_data.lock);
        DEBUG_DESTRUCT_LOCK(&myquery_data.lock);

        /* we should have received back two info structs, one containing
         * a comma-delimited list of PMIx spawn attributes the RM supports,
         * and the other containing a comma-delimited list of PMIx debugger
         * attributes it supports */
        if (2 != myquery_data.ninfo) {
            /* this is an error */
            fprintf(stderr, "PMIx Query returned an incorrect number of results: %lu\n", myquery_data.ninfo);
            PMIX_INFO_FREE(myquery_data.info, myquery_data.ninfo);
            goto done;
        }

        /* we would like to co-spawn the debugger daemons with the app, but
         * let's first check to see if this RM supports that operation by
         * looking for the PMIX_COSPAWN_APP attribute in the spawn support
         *
         * We will also check to see if "stop_on_exec" is supported. Few RMs
         * do so, which is why we have to check. The reference server sadly is
         * not one of them, so we shouldn't find it here
         *
         * Note that the PMIx reference server always returns the query results
         * in the same order as the query keys. However, this is not guaranteed,
         * so we should search the returned info structures to find the desired key
         */
        for (n=0; n < myquery_data.ninfo; n++) {
            if (0 == strcmp(myquery_data.info[n].key, PMIX_QUERY_SPAWN_SUPPORT)) {
                /* see if the cospawn attribute is included */
                if (NULL != strstr(myquery_data.info[n].value.data.string, PMIX_COSPAWN_APP)) {
                    cospawn = true;
                } else {
                    cospawn = false;
                }
            } else if (0 == strcmp(myquery_data.info[n].key, PMIX_QUERY_DEBUG_SUPPORT)) {
                if (NULL != strstr(myquery_data.info[n].value.data.string, PMIX_DEBUG_STOP_ON_EXEC)) {
                    stop_on_exec = true;
                } else {
                    stop_on_exec = false;
                }
            }
        }

        /* if cospawn is available and they requested it, then we launch both
         * the app and the debugger daemons at the same time */
        if (cospawn && cospawn_reqd) {

        } else {
            /* we must do these as separate launches, so do the app first */
            napps = 1;
            PMIX_APP_CREATE(app, napps);
            /* setup the executable */
            app[0].cmd = strdup("hello");
            PMIX_ARGV_APPEND(rc, app[0].argv, "./hello");
            getcwd(cwd, 1024);  // point us to our current directory
            app[0].cwd = strdup(cwd);
            app[0].maxprocs = 2;
            /* provide job-level directives so the apps do what the user requested */
            ninfo = 5;
            PMIX_INFO_CREATE(info, ninfo);
            PMIX_INFO_LOAD(&info[0], PMIX_MAPBY, "slot", PMIX_STRING);  // map by slot
            if (stop_on_exec) {
                PMIX_INFO_LOAD(&info[1], PMIX_DEBUG_STOP_ON_EXEC, NULL, PMIX_BOOL);  // procs are to stop on first instruction
            } else {
                PMIX_INFO_LOAD(&info[1], PMIX_DEBUG_STOP_IN_INIT, NULL, PMIX_BOOL);  // procs are to pause in PMIx_Init for debugger attach
            }
            PMIX_INFO_LOAD(&info[2], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);  // forward stdout to me
            PMIX_INFO_LOAD(&info[3], PMIX_FWD_STDERR, NULL, PMIX_BOOL);  // forward stderr to me
            PMIX_INFO_LOAD(&info[4], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL); // notify us when the job completes

            /* spawn the job - the function will return when the app
             * has been launched */
            fprintf(stderr, "Debugger: spawning %s\n", app[0].cmd);
            if (PMIX_SUCCESS != (rc = PMIx_Spawn(info, ninfo, app, napps, clientspace))) {
                fprintf(stderr, "Application failed to launch with error: %s(%d)\n", PMIx_Error_string(rc), rc);
                goto done;
            }
            PMIX_INFO_FREE(info, ninfo);
            PMIX_APP_FREE(app, napps);

            /* get the proctable for this nspace */
            PMIX_QUERY_CREATE(query, 1);
            PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_PROC_TABLE);
            query[0].nqual = 1;
            PMIX_INFO_CREATE(query->qualifiers, query[0].nqual);
            PMIX_INFO_LOAD(&query->qualifiers[0], PMIX_NSPACE, clientspace, PMIX_STRING);

            DEBUG_CONSTRUCT_LOCK(&myquery_data.lock);
            myquery_data.info = NULL;
            myquery_data.ninfo = 0;

            if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(query, 1, cbfunc, (void*)&myquery_data))) {
                fprintf(stderr, "Debugger[%s:%d] Proctable query failed: %d\n", myproc.nspace, myproc.rank, rc);
                goto done;
            }
            /* wait to get a response */
            DEBUG_WAIT_THREAD(&myquery_data.lock);
            DEBUG_DESTRUCT_LOCK(&myquery_data.lock);
            /* we should have gotten a response */
            if (PMIX_SUCCESS != myquery_data.status) {
                fprintf(stderr, "Debugger[%s:%d] Proctable query failed: %s\n",
                        myproc.nspace, myproc.rank, PMIx_Error_string(myquery_data.status));
                goto done;
            }
            /* there should have been data */
            if (NULL == myquery_data.info || 0 == myquery_data.ninfo) {
                fprintf(stderr, "Debugger[%s:%d] Proctable query return no results\n",
                        myproc.nspace, myproc.rank);
                goto done;
            }
            /* the query should have returned a data_array */
            if (PMIX_DATA_ARRAY != myquery_data.info[0].value.type) {
                fprintf(stderr, "Debugger[%s:%d] Query returned incorrect data type: %s(%d)\n",
                        myproc.nspace, myproc.rank,
                        PMIx_Data_type_string(myquery_data.info[0].value.type),
                        (int)myquery_data.info[0].value.type);
                return -1;
            }
            if (NULL == myquery_data.info[0].value.data.darray->array) {
                fprintf(stderr, "Debugger[%s:%d] Query returned no proctable info\n");
                goto done;
            }
            /* the data array consists of a struct:
             *     size_t size;
             *     void* array;
             *
             * In this case, the array is composed of pmix_proc_info_t structs:
             *     pmix_proc_t proc;   // contains the nspace,rank of this proc
             *     char* hostname;
             *     char* executable_name;
             *     pid_t pid;
             *     int exit_code;
             *     pmix_proc_state_t state;
             */

            /* now launch the debugger daemons */
            if (PMIX_SUCCESS != (rc = spawn_debugger(clientspace, &dbrel))) {
                fprintf(stderr, "Debugger daemons failed to spawn: %s\n", PMIx_Error_string(rc));
                goto done;
            }
        }
    }

  rundebugger:
    /* this is where a debugger tool would wait until the debug operation is complete */
    DEBUG_WAIT_THREAD(&dbrel.lock);
    DEBUG_WAIT_THREAD(&myrel.lock);

  done:
    DEBUG_DESTRUCT_LOCK(&myrel.lock);
    DEBUG_DESTRUCT_LOCK(&dbrel.lock);
    PMIx_tool_finalize();

    return(rc);
}

static int attach_to_running_job(char *nspace)
{
    pmix_status_t rc;
    pmix_proc_t myproc;
    pmix_query_t *query;
    size_t nq;
    myquery_data_t *q;

    /* query the active nspaces so we can verify that the
     * specified one exists */
    nq = 1;
    PMIX_QUERY_CREATE(query, nq);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_NAMESPACES);

    q = (myquery_data_t*)malloc(sizeof(myquery_data_t));
    DEBUG_CONSTRUCT_LOCK(&q->lock);
    if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(query, nq, cbfunc, (void*)q))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Query_info failed: %d\n", myproc.nspace, myproc.rank, rc);
        return -1;
    }
    DEBUG_WAIT_THREAD(&q->lock);
    DEBUG_DESTRUCT_LOCK(&q->lock);

    if (NULL == q->info) {
        fprintf(stderr, "Query returned no info\n");
        return -1;
    }
    /* the query should have returned a comma-delimited list of nspaces */
    if (PMIX_STRING != q->info[0].value.type) {
        fprintf(stderr, "Query returned incorrect data type: %d\n", q->info[0].value.type);
        return -1;
    }
    if (NULL == q->info[0].value.data.string) {
        fprintf(stderr, "Query returned no active nspaces\n");
        return -1;
    }

    fprintf(stderr, "Query returned %s\n", q->info[0].value.data.string);
    return 0;

#if 0
    /* split the returned string and look for the given nspace */

    /* if not found, then we have an error */
    PMIX_INFO_FREE(info, ninfo);

    /* get the proctable for this nspace */
    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    (void)strncpy(info[0].key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
    (void)strncpy(info[0].qualifier, nspace, PMIX_MAX_KEYLEN);
    if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(info, ninfo, infocbfunc, (void*)&active))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Query_info_nb failed: %d\n", myproc.nspace, myproc.rank, rc);
        return -1;
    }
    /* wait to get a response */

    /* the query should have returned a data_array */
    if (PMIX_DATA_ARRAY != info[0].type) {
        fprintf(stderr, "Query returned incorrect data type: %d\n", info[0].type);
        return -1;
    }
    if (NULL == info[0].data.darray.array) {
        fprintf(stderr, "Query returned no proctable info\n");
        return -1;
    }
    /* the data array consists of a struct:
     *     size_t size;
     *     void* array;
     *
     * In this case, the array is composed of pmix_proc_info_t structs:
     *     pmix_proc_t proc;   // contains the nspace,rank of this proc
     *     char* hostname;
     *     char* executable_name;
     *     pid_t pid;
     *     int exit_code;
     *     pmix_proc_state_t state;
     */

    /* this is where a debugger tool would process the proctable to
     * create whatever blob it needs to provide to its daemons */
    PMIX_INFO_FREE(info, ninfo);

    /* setup the debugger daemon spawn request */
    napps = 1;
    PMIX_APP_CREATE(app, napps);
    /* setup the name of the daemon executable to launch */
    app[0].cmd = strdup("debuggerdaemon");
    app[0].argc = 1;
    app[0].argv = (char**)malloc(2*sizeof(char*));
    app[0].argv[0] = strdup("debuggerdaemon");
    app[0].argv[1] = NULL;
    /* provide directives so the daemons go where we want, and
     * let the RM know these are debugger daemons */
    ninfo = 3;
    PMIX_INFO_CREATE(app[0].info, ninfo);
    PMIX_INFO_LOAD(&app[0].info[0], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);  // instruct the RM to launch one copy of the executable on each node
    PMIX_INFO_LOAD(&app[0].info[1], PMIX_DEBUGGER_DAEMONS, true, PMIX_BOOL); // these are debugger daemons
    PMIX_INFO_LOAD(&app[0].info[2], PMIX_DEBUG_TARGET, nspace, PMIX_STRING); // the "jobid" of the application to be debugged

    /* spawn the daemons */
    PMIx_Spawn(NULL, 0, app, napps, dspace);
    /* cleanup */
    PMIX_APP_FREE(app, napps);

    /* this is where a debugger tool would wait until the debug operation is complete */

    return 0;
#endif
}
