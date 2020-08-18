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
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <pmix_tool.h>

#include "debugger.h"

static pmix_proc_t myproc;
static bool stop_on_exec = false;
static char client_nspace[PMIX_MAX_NSLEN+1];
static char daemon_nspace[PMIX_MAX_NSLEN+1];

/* This is a callback function for the PMIx_Query_info
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
static void query_cbfunc(pmix_status_t status,
                         pmix_info_t *info, size_t ninfo,
                         void *cbdata,
                         pmix_release_cbfunc_t release_fn,
                         void *release_cbdata)
{
    myquery_data_t *mq = (myquery_data_t*)cbdata;
    size_t n;

    printf("Called %s as callback for PMIx_Query\n", __FUNCTION__);
    mq->status = status;
    /* save the returned info - the PMIx library "owns" it
     * and will release it and perform other cleanup actions
     * when release_fn is called */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(mq->info, ninfo);
        mq->ninfo = ninfo;
        for (n=0; n < ninfo; n++) {
            printf("Key %s Type %s(%d)\n", info[n].key,
                   PMIx_Data_type_string(info[n].value.type),
                   info[n].value.type);
            PMIX_INFO_XFER(&mq->info[n], &info[n]);
        }
    }

    /* let the library release the data and cleanup from
     * the operation */
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }

    /* Release the lock */
    DEBUG_WAKEUP_THREAD(&mq->lock);
}

/* This is the event notification function we pass down below
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
    myrel_t *lock = NULL;
    size_t n;

    printf("%s called as callback for event=%s\n", __FUNCTION__,
           PMIx_Error_string(status));
    if (PMIX_ERR_UNREACH == status ||
        PMIX_ERR_LOST_CONNECTION_TO_SERVER == status) {
        /* We should always have info returned to us - if not, there is
         * nothing we can do */
        if (NULL != info) {
            for (n=0; n < ninfo; n++) {
                if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_RETURN_OBJECT)) {
                    lock = (myrel_t*)info[n].value.data.ptr;
                }
            }
        }

        /* Save the exit status */
        if (NULL != lock) {
            lock->exit_code = status;
            lock->exit_code_given = true;
            /* Always release the lock if we lose connection to our host
             * server */
            DEBUG_WAKEUP_THREAD(&lock->lock);
        }
    }

    /* This example doesn't do anything with default events */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/* This is an event notification function that we explicitly request
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
    bool found;
    int exit_code;
    size_t n;
    pmix_proc_t *affected = NULL;

    printf("%s called as callback for event=%s source=%s:%d\n", __FUNCTION__,
           PMIx_Error_string(status), source->nspace, source->rank);
    /* find the return object */
    lock = NULL;
    found = false;
    for (n=0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT,
                         PMIX_MAX_KEYLEN)) {
            /* Save the lock that was passed to this callback */
            lock = (myrel_t*)info[n].value.data.ptr;

            /* not every RM will provide an exit code, but check if one was
             * given */
        } else if (0 == strncmp(info[n].key, PMIX_EXIT_CODE, PMIX_MAX_KEYLEN)) {
            exit_code = info[n].value.data.integer;
            found = true;
        } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC,
                                PMIX_MAX_KEYLEN)) {
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

    printf("DEBUGGER NOTIFIED THAT JOB %s TERMINATED \n",
            (NULL == affected) ? "NULL" : affected->nspace);
    if (found) {
        if (!lock->exit_code_given) {
            lock->exit_code = exit_code;
            lock->exit_code_given = true;
        }
    }

    /* A system PMIx daemon may have kept track of notifications for
     * termination of previous application runs, and may send those
     * notifications to this process, which has registered a callback for
     * application terminations. Those notifcations need to be ignored.
     *
     * Therefore, in the co-spawn case, we expect one termination notification,
     * which is for the combined application/daemon namespace when the daemon
     * terminates.
     *
     * In the separate spawn case, we expect two terminations, the application
     * and the daemon. */
    if ((0 == strcmp(daemon_nspace, source->nspace)) ||
        (0 == strcmp(client_nspace, source->nspace))) {
        lock->lock.count--;
        if (0 == lock->lock.count) {
            DEBUG_WAKEUP_THREAD(&lock->lock);
        }
    }

    /* tell the event handler state machine that we are the last step */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    return;
}

/* Event handler registration is done asynchronously because it
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

    printf("%s called to register callback\n", __FUNCTION__);
    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                myproc.nspace, myproc.rank, status,
                (unsigned long)evhandler_ref);
    }
    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

static int cospawn_launch(myrel_t *myrel) {
    pmix_info_t *info;
    pmix_app_t *app;
    size_t ninfo;
    int code = PMIX_ERR_JOB_TERMINATED;
    pmix_status_t rc;
    int n;
    pmix_data_array_t data_array;
    mylock_t mylock;
    pmix_proc_t daemon_proc;
    char cwd[_POSIX_PATH_MAX + 1];

    printf("Calling %s to spawn application processes and debugger daemon\n",
           __FUNCTION__);
    /* Provide job-level directives so the apps do what the user requested.
     * These attributes apply to both the application and daemon processes. */
    ninfo = 4;
    n = 0;
    PMIX_INFO_CREATE(info, ninfo);
    /* Forward stdout to this process */
    PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);
    n++;
    /* Forward stderr to this process */
    PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDERR, NULL, PMIX_BOOL);
    n++;
    /* Process that is spawning processes is a tool process */
    PMIX_INFO_LOAD(&info[n], PMIX_REQUESTOR_IS_TOOL, NULL, PMIX_BOOL);
    n++;
    /* Map spawned processes by slot */
    PMIX_INFO_LOAD(&info[n], PMIX_MAPBY, "slot", PMIX_STRING);

    /* The application and daemon processes are being spawned together
     * so create 2 pmix_app_t structures. The first is parameters for
     * the application and the second is parameters for the demon. */
    PMIX_APP_CREATE(app, 2);
    /* setup the executable */
    app[0].cmd = strdup("./hello");
    /* Set up the executable command arguments, in this case just the
     * application (argv[0]) */
    PMIX_ARGV_APPEND(rc, app->argv, app[0].cmd);
    app[0].env = NULL;
    /* Set the working directory */
    getcwd(cwd, _POSIX_PATH_MAX);
    app[0].cwd = strdup(cwd);
    /* Two application processes */
    app[0].maxprocs = 2;

    app[0].ninfo = 1;
    PMIX_INFO_CREATE(app[0].info, app[0].ninfo);
    n = 0;
    if (stop_on_exec) {
        /* Stop application at first instruction */
        PMIX_INFO_LOAD(&app[n].info[0], PMIX_DEBUG_STOP_ON_EXEC, NULL,
                       PMIX_BOOL);
    } else {
        /* Stop application in PMIx_Init */
        PMIX_INFO_LOAD(&app[n].info[0], PMIX_DEBUG_STOP_IN_INIT, NULL,
                       PMIX_BOOL);
    }

    /* Set up the daemon executable */
    app[1].cmd = strdup("./daemon");
    /* Set up daemon arguments, in this case just the executable (argv[0]) */
    PMIX_ARGV_APPEND(rc, app[1].argv, app[1].cmd);
    app[1].env = NULL;
    /* Set the working directory */
    getcwd(cwd, _POSIX_PATH_MAX);
    app[1].cwd = strdup(cwd);
    /* One deamon process */
    app[1].maxprocs = 1;
    /* provide directives so the daemons go where we want, and
     * let the RM know these are debugger daemons */
    app[1].ninfo = 3;
    n = 0;
    PMIX_INFO_CREATE(app[1].info, app[1].ninfo);
    /* This process is a debugger daemon */
    PMIX_INFO_LOAD(&app[1].info[n], PMIX_DEBUGGER_DAEMONS, NULL, PMIX_BOOL);
    n++;
    /* Notify this process when debugger job completes */
    PMIX_INFO_LOAD(&app[1].info[n], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL);
    n++;
    /* Tell daemon that application is waiting for daemon to relase it */
    PMIX_INFO_LOAD(&app[1].info[n], PMIX_DEBUG_WAITING_FOR_NOTIFY, NULL,
                   PMIX_BOOL); 
    /* spawn the job - the function will return when the app
     * has been launched */
    rc = PMIx_Spawn(info, ninfo, app, 2, client_nspace);
    myrel->lock.count = 1; //app[0].maxprocs + app[1].maxprocs;
    myrel->nspace = strdup(client_nspace);
    PMIX_INFO_FREE(info, ninfo);
    PMIX_APP_FREE(app, 2);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Application failed to launch with error: %s(%d)\n",
                PMIx_Error_string(rc), rc);
        return rc;
    }
    /* Daemon and application are in same namespace */
    printf("Application namespace is %s\n", client_nspace);
    /* Register the termination event handler here with the intent to
     * filter out non-daemon notifcations .
     * Since the daemon is in the same namespace as the application, it's
     * rank is assigned one higher than the last application process. In 
     * this example,the daemon's rank is 2.
     */
    strcpy(daemon_proc.nspace, client_nspace);
    strcpy(daemon_nspace, client_nspace);
    daemon_proc.rank = 2;
    data_array.size = 1;
    data_array.type = PMIX_PROC;
    data_array.array = &daemon_proc;
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_CUSTOM_RANGE, &data_array,
                   PMIX_DATA_ARRAY);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_RETURN_OBJECT, myrel, PMIX_POINTER);
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, info, 2, release_fn,
                                evhandler_reg_callbk, (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(info, 2);
    return rc;
}

static pmix_status_t spawn_debugger(char *appspace, myrel_t *myrel)
{
    pmix_status_t rc;
    pmix_info_t *dinfo;
    pmix_app_t *debugger;
    size_t dninfo;
    int n;
    char cwd[1024];
    mylock_t mylock;
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;
    pmix_proc_t proc;

    printf("Calling %s to spawn the debugger daemon\n", __FUNCTION__);
    /* Setup the debugger  spawn parameters*/
    PMIX_APP_CREATE(debugger, 1);
    debugger[0].cmd = strdup("./daemon");
    /* Set up debugger command arguments, in this example, just argv[0] */
    PMIX_ARGV_APPEND(rc, debugger[0].argv, "./daemon");
    /* No environment variables */
    debugger[0].env = NULL;
    /* Set the working directory to our current directory */
    getcwd(cwd, 1024);
    debugger[0].cwd = strdup(cwd);
    /* Spawn one daemon process */
    debugger[0].maxprocs = 1;
    /* No spawn attributes set here, all are set in dinfo array */
    debugger[0].ninfo = 0;
    debugger[0].info = NULL;
    /* Set attributes for debugger daemon launch and let the RM know these are
     * debugger daemons */
    dninfo = 7;
    n = 0;
    PMIX_INFO_CREATE(dinfo, dninfo);
#if 0
    /* Co-locate daemons 1 per node -- Not implemented yet! */
    PMIX_INFO_LOAD(&dinfo[n], PMIX_DEBUG_DAEMONS_PER_NODE, 1, PMIX_UINT16);
#else
    /* Launch one daemon per node */
    PMIX_INFO_LOAD(&dinfo[n], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);
#endif
    n++;
    /* Indicate a debugger daemon is being spawned */
    PMIX_INFO_LOAD(&dinfo[n], PMIX_DEBUGGER_DAEMONS, NULL, PMIX_BOOL);
    n++;
    /* Indicate that we want to target the application namespace (Replacement for deprecated PMIX_DEBUG_JOB) */
#if 0
    PMIX_INFO_LOAD(&dinfo[n], PMIX_DEBUG_TARGET, appspace, PMIX_STRING);
#else
    PMIX_INFO_LOAD(&dinfo[n], PMIX_DEBUG_JOB, appspace, PMIX_STRING);
#endif
    n++;
    /* Notify this process when the job completes */
    PMIX_INFO_LOAD(&dinfo[n], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL);
    n++;
    /* Tell debugger demon application processes are waiting to be released */
    PMIX_INFO_LOAD(&dinfo[n], PMIX_DEBUG_WAITING_FOR_NOTIFY, NULL, PMIX_BOOL);
    n++;
    /* Forward stdout to this process */
    PMIX_INFO_LOAD(&dinfo[n], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);
    n++;
    /* Forward stderr to this process */
    PMIX_INFO_LOAD(&dinfo[n], PMIX_FWD_STDERR, NULL, PMIX_BOOL);
    /* spawn the daemons */
    printf("Debugger: spawning %s\n", debugger[0].cmd);
    rc = PMIx_Spawn(dinfo, dninfo, debugger, 1, daemon_nspace);
    /* Unconditionally clean up allocated structures */
    PMIX_INFO_FREE(dinfo, dninfo);
    PMIX_APP_FREE(debugger, 1);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Debugger daemon failed to launch error= %s\n",
                PMIx_Error_string(rc));
        return rc;
    }

    /* register callback for when this job terminates */
    myrel->nspace = strdup(daemon_nspace);
    dninfo = 2;
    PMIX_INFO_CREATE(dinfo, dninfo);
    n = 0;
    /* only call me back when this specific job terminates */
    PMIX_LOAD_PROCID(&proc, daemon_nspace, PMIX_RANK_WILDCARD);
    /* Specify the lock passed to the event registration callback */
    PMIX_INFO_LOAD(&dinfo[n], PMIX_EVENT_RETURN_OBJECT, myrel, PMIX_POINTER);
    n++;
    /* Specify the proc that we want termination notification for */
    PMIX_INFO_LOAD(&dinfo[n], PMIX_EVENT_AFFECTED_PROC, &proc, PMIX_PROC);
    /* Increment lock count to indicate we need to wait for both the debugger
     * and application tasks to terminate. */
    myrel->lock.count++;

    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, dinfo, dninfo,
                                release_fn, evhandler_reg_callbk,
                                (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    printf("Debugger registered for termination of nspace %s\n", daemon_nspace);
    rc = mylock.status;
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(dinfo, dninfo);

    return rc;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t *info;
    pmix_app_t *app;
    size_t ninfo;
    size_t napps;
    int i;
    int n;
    pmix_query_t *query;
    size_t nq;
    myquery_data_t myquery_data;
    bool cospawn = false;
    bool cospawn_reqd = false;
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;
    mylock_t mylock;
    myrel_t myrel;
    pid_t pid;
    pmix_proc_t proc;
    char cwd[1024];

    pid = getpid();

    /* Process any command line arguments we were given */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "-h") ||
            0 == strcmp(argv[i], "--help")) {
            /* print the usage message and exit */
            printf("Usage: ./direct [-c|--cospawn]\n");
            printf(" Requires 'prte' persistent daemon is running.\n");
            printf(" -h|--help      Display this help message and exit.\n");
            printf(" -c|--cospawn   Use the PMIx Cospawn technique to launch app and daemons\n");
            exit(0);
        }
        if (0 == strcmp(argv[i], "-c") ||
            0 == strcmp(argv[i], "--cospawn")){
            cospawn_reqd = true;
            break;
        }
    }
    info = NULL;
    ninfo = 2;
    n = 0;

    /* Use the system connection first, if available */
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[n], PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    n++;
    PMIX_INFO_LOAD(&info[n], PMIX_LAUNCHER, NULL, PMIX_BOOL);
    /* Initialize as a tool */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, 1))) {
        fprintf(stderr, "PMIx_tool_init failed: %s(%d)\n",
                PMIx_Error_string(rc), rc);
        exit(rc);
    }
    PMIX_INFO_FREE(info, 1);

    printf("Debugger ns %s rank %d pid %lu: Running\n", myproc.nspace,
           myproc.rank, (unsigned long)pid);

    /* Construct my own release first */
    DEBUG_CONSTRUCT_LOCK(&myrel.lock);

    /* register a default event handler */
    ninfo = 1;
    n = 0;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_RETURN_OBJECT, &myrel, PMIX_POINTER);
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(NULL, 0, info, ninfo,
                                notification_fn, evhandler_reg_callbk,
                                (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(info, ninfo);

    /* This is an initial launch - we need to launch the application
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
    /* Set up the caddy to retrieve the data */
    DEBUG_CONSTRUCT_LOCK(&myquery_data.lock);
    myquery_data.info = NULL;
    myquery_data.ninfo = 0;
    /* Execute the query */
    if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(query, nq, query_cbfunc,
                                                 (void*)&myquery_data))) {
        fprintf(stderr, "PMIx_Query_info failed: %d\n", rc);
        goto done;
    }
    DEBUG_WAIT_THREAD(&myquery_data.lock);
    DEBUG_DESTRUCT_LOCK(&myquery_data.lock);

    /* We should have received back two info structs, one containing
     * a comma-delimited list of PMIx spawn attributes the RM supports,
     * and the other containing a comma-delimited list of PMIx debugger
     * attributes it supports */
    if (2 != myquery_data.ninfo) {
        fprintf(stderr,
                "PMIx Query returned an incorrect number of results: %lu\n",
                myquery_data.ninfo);
        PMIX_INFO_FREE(myquery_data.info, myquery_data.ninfo);
        goto done;
    }

    /* We would like to co-spawn the debugger daemons with the app, but
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
            /* See if the cospawn attribute is included */
            if (NULL != strstr(myquery_data.info[n].value.data.string,
                               PMIX_COSPAWN_APP)) {
                cospawn = true;
            } else {
                cospawn = false;
            }
        } else if (0 == strcmp(myquery_data.info[n].key,
                               PMIX_QUERY_DEBUG_SUPPORT)) {
            if (NULL != strstr(myquery_data.info[n].value.data.string,
                               PMIX_DEBUG_STOP_ON_EXEC)) {
                stop_on_exec = true;
            } else {
                stop_on_exec = false;
            }
        }
    }

    /* If cospawn is available and the user requested it, then we launch both
     * the app and the debugger daemons at the same time */
    if (cospawn && cospawn_reqd) {
        cospawn_launch(&myrel);
    } else {
        /* We must do these as separate launches, so do the app first */
        napps = 1;
        PMIX_APP_CREATE(app, napps);
        /* Set up the application executable */
        app[0].cmd = strdup("./hello");
        /* Set the application command arguments, in this case just argv[0] */
        PMIX_ARGV_APPEND(rc, app[0].argv, "./hello");
        /* Set the applictaion working directory to our current directory */
        getcwd(cwd, 1024);
        app[0].cwd = strdup(cwd);
        /* Two application processes */
        app[0].maxprocs = 2;
        /* No attributes specified in pmix_app_t structure */
        app[0].ninfo = 0;
        /* Set application spawn attributes for PMIx_Spawn call */
        ninfo = 5;
        n = 0;
        PMIX_INFO_CREATE(info, ninfo);
        /* Map application processes by slot */
        PMIX_INFO_LOAD(&info[n], PMIX_MAPBY, "slot", PMIX_STRING);
        n++;
        if (stop_on_exec) {
            /* Pause application at first instruction */
            PMIX_INFO_LOAD(&info[n], PMIX_DEBUG_STOP_ON_EXEC, NULL, PMIX_BOOL);
        } else {
            /* Pause application in PMIx_Init */
            PMIX_INFO_LOAD(&info[n], PMIX_DEBUG_STOP_IN_INIT, NULL, PMIX_BOOL);
        }
        n++;
        /* Forward application stdout to this process */
        PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDOUT, NULL, PMIX_BOOL);
        n++;
        /* Forward application stderr to this process */
        PMIX_INFO_LOAD(&info[n], PMIX_FWD_STDERR, NULL, PMIX_BOOL);
        n++;
        /* Notify this process when the application terminates */
        PMIX_INFO_LOAD(&info[n], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL);

        /* Spawn the job - the function will return when the app has been
         * launched */
        printf("Debugger: spawning %s\n", app[0].cmd);
        if (PMIX_SUCCESS != (rc = PMIx_Spawn(info, ninfo, app, napps,
                                             client_nspace))) {
            fprintf(stderr, "Application failed to launch with error: %s(%d)\n",
                    PMIx_Error_string(rc), rc);
            goto done;
        }
        PMIX_INFO_FREE(info, ninfo);
        PMIX_APP_FREE(app, napps);

        /* Register callback for when the application terminates */
        /* Only call me back when this specific job terminates */
        PMIX_LOAD_PROCID(&proc, client_nspace, PMIX_RANK_WILDCARD);
        ninfo = 2;
        n = 0;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[n], PMIX_EVENT_RETURN_OBJECT, &myrel, PMIX_POINTER);
        n++;
        PMIX_INFO_LOAD(&info[n], PMIX_EVENT_AFFECTED_PROC, &proc, PMIX_PROC);
        /* Track number of jobs to terminate */
        myrel.lock.count++;

        DEBUG_CONSTRUCT_LOCK(&mylock);
        PMIx_Register_event_handler(&code, 1, info, ninfo, release_fn,
                                    evhandler_reg_callbk, (void*)&mylock);
        DEBUG_WAIT_THREAD(&mylock);
        printf("Debugger registered for termination on nspace %s\n",
               client_nspace);
        rc = mylock.status;
        DEBUG_DESTRUCT_LOCK(&mylock);
        PMIX_INFO_FREE(info, ninfo);

        /* Get the proctable for the application namespace */
        PMIX_QUERY_CREATE(query, 1);
        PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_PROC_TABLE);
        query[0].nqual = 1;
        PMIX_INFO_CREATE(query->qualifiers, query[0].nqual);
        PMIX_INFO_LOAD(&query->qualifiers[0], PMIX_NSPACE, client_nspace,
                       PMIX_STRING);

        DEBUG_CONSTRUCT_LOCK(&myquery_data.lock);
        myquery_data.info = NULL;
        myquery_data.ninfo = 0;

        if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(query, 1, query_cbfunc,
                                                     (void*)&myquery_data))) {
            fprintf(stderr, "Debugger[%s:%d] Proctable query failed: %d\n",
                    myproc.nspace, myproc.rank, rc);
            goto done;
        }
        /* Wait to get a response */
        DEBUG_WAIT_THREAD(&myquery_data.lock);
        DEBUG_DESTRUCT_LOCK(&myquery_data.lock);
        /* we should have gotten a response */
        if (PMIX_SUCCESS != myquery_data.status) {
            fprintf(stderr, "Debugger[%s:%d] Proctable query failed: %s\n",
                    myproc.nspace, myproc.rank,
                    PMIx_Error_string(myquery_data.status));
            goto done;
        }
        /* There should have been data */
        if (NULL == myquery_data.info || 0 == myquery_data.ninfo) {
            fprintf(stderr,
                    "Debugger[%s:%d] Proctable query return no results\n",
                    myproc.nspace, myproc.rank);
            goto done;
        }
        /* The query should have returned a data_array */
        if (PMIX_DATA_ARRAY != myquery_data.info[0].value.type) {
            fprintf(stderr, "Debugger[%s:%d] Query returned incorrect data type: %s(%d)\n",
                    myproc.nspace, myproc.rank,
                    PMIx_Data_type_string(myquery_data.info[0].value.type),
                    (int)myquery_data.info[0].value.type);
            return -1;
        }
        if (NULL == myquery_data.info[0].value.data.darray->array) {
            fprintf(stderr, "Debugger[%s:%d] Query returned no proctable info\n",
                    myproc.nspace, myproc.rank);
            goto done;
        }
        /* The data array consists of a struct:
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
        printf("Received proc table for %d procs\n",
               (int)myquery_data.info[0].value.data.darray->size);
        /* Now launch the debugger daemons */
        if (PMIX_SUCCESS != (rc = spawn_debugger(client_nspace, &myrel))) {
            fprintf(stderr, "Debugger daemons failed to spawn: %s\n",
                    PMIx_Error_string(rc));
            goto done;
        }
    }

    /* This is where a debugger tool would wait until the debug operation is
     * complete */
    DEBUG_WAIT_THREAD(&myrel.lock);

  done:
    DEBUG_DESTRUCT_LOCK(&myrel.lock);
    PMIx_tool_finalize();
    return(rc);
}
