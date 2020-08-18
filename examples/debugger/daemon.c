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
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include <pmix_tool.h>
#include "debugger.h"

/*
 * This module is an example of a PMIx debugger daemon. The debugger daemon
 * handles interactions with application processes on a node in behalf of the
 * front end debugger process.
 */

static pmix_proc_t myproc;
static char *target_namespace = NULL;

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
    printf("%s called as default event handler for event=%s\n", __FUNCTION__,
           PMIx_Error_string(status));

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

    printf("%s called as callback for event=%s\n", __FUNCTION__,
           PMIx_Error_string(status));

    /* Be sure notification is for our application process namespace */
    if (0 != strcmp(target_namespace, source->nspace)) {
        printf("Ignoring termination notification for '%s'\n", source->nspace);
        /* tell the event handler state machine that we are the last step */
        if (NULL != cbfunc) {
            cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
            return;
        }
    }
    /* find our return object */
    lock = NULL;
    found = false;
    for (n=0; n < ninfo; n++) {
        /* Retrieve the lock that needs to be released by this callback. */
        if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT,
                         PMIX_MAX_KEYLEN)) {
            lock = (myrel_t*)info[n].value.data.ptr;
            /* Not every RM will provide an exit code, but check if one was
             * given */
        } else if (0 == strncmp(info[n].key, PMIX_EXIT_CODE, PMIX_MAX_KEYLEN)) {
            exit_code = info[n].value.data.integer;
            found = true;
        } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC,
                                PMIX_MAX_KEYLEN)) {
            affected = info[n].value.data.proc;
        }
    }
    /* if the lock object wasn't returned, then that is an error */
    if (NULL == lock) {
        fprintf(stderr, "LOCK WASN'T RETURNED IN RELEASE CALLBACK\n");
        /* let the event handler progress */
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }

    printf("DEBUGGER DAEMON NAMESPACE %s NOTIFIED THAT JOB TERMINATED - AFFECTED %s\n",
           lock->nspace, (NULL == affected) ? "NULL" : affected->nspace);

    /* If the lock object was found then store return status in the lock
     * object. */
    if (found) {
        lock->exit_code = exit_code;
        lock->exit_code_given = true;
    }

    /* tell the event handler state machine that we are the last step */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }

    /* Wake up the thread that is waiting for this callback to complete */
    DEBUG_WAKEUP_THREAD(&lock->lock);
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

    printf("%s called as registration callback\n", __FUNCTION__);

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                   myproc.nspace, myproc.rank, status,
                   (unsigned long)evhandler_ref);
    }
    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val;
    pmix_proc_t wildproc;
    pmix_info_t *info;
    size_t ninfo;
    pmix_query_t *query;
    pmix_proc_info_t *proctable;
    size_t nq;
    size_t n;
    pmix_info_t *query_data = NULL;
    size_t query_size = 0;
    pid_t pid;
    pmix_status_t code = PMIX_ERR_JOB_TERMINATED;
    mylock_t mylock;
    myrel_t myrel;
    uint16_t localrank;
    int i;
    int cospawned_namespace = 0;

    pid = getpid();

    /* Initialize this daemon - since we were launched by the RM, our
     * connection info will have been provided at startup. */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, NULL, 0))) {
        fprintf(stderr, "Debugger daemon: PMIx_tool_init failed: %s\n",
                PMIx_Error_string(rc));
        exit(0);
    }
    printf("Daemon: Debugger daemon ns %s rank %d pid %lu: Running\n", myproc.nspace,
           myproc.rank, (unsigned long)pid);

    /* Register our default event handler */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0,
                                notification_fn, evhandler_reg_callbk,
                                (void*)&mylock);
    DEBUG_WAIT_THREAD(&mylock);
    if (PMIX_SUCCESS != mylock.status) {
        rc = mylock.status;
        DEBUG_DESTRUCT_LOCK(&mylock);
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);

    /*
     * Get the namespace of the job we are to debug. If the application and the
     * debugger daemons are spawned separately or if the debugger is attaching
     * to a running application, the debugger will set the application
     * namespace in the PMIX_DEBUG_JOB attribute, and the daemon retrieves
     * it by calling PMIx_Get.
     *
     * If the application processes and debugger daemons are spawned together
     * (cospawn), then the debugger cannot pass the application namespace since
     * that is not known until after the PMIx_Spawn call completes. However,
     * the applicaton processes and the debugger daemons have the same
     * namespace, so this module uses the debugger namespace, which it knows.
     */
#ifdef PMIX_LOAD_PROCID
    PMIX_LOAD_PROCID(&wildproc, myproc.nspace, PMIX_RANK_WILDCARD);
#else
    PMIX_PROC_CONSTRUCT(&wildproc);
    (void)strncpy(wildproc.nspace, myproc.nspace, PMIX_MAX_KEYLEN);
    wildproc.rank = PMIX_RANK_WILDCARD;
#endif
    rc = PMIx_Get(&wildproc, PMIX_DEBUG_JOB, NULL, 0, &val);
    if (PMIX_ERR_NOT_FOUND == rc) {
        /* Save the application namespace for later */
        target_namespace = strdup(myproc.nspace);
        cospawned_namespace = 1; // Note: this cospawn bug should be fixed
    } else if (rc != PMIX_SUCCESS) {
        fprintf(stderr,
                "[%s:%d:%lu] Failed to get job being debugged - error %s\n",
                myproc.nspace, myproc.rank, (unsigned long) pid,
                PMIx_Error_string(rc));
        goto done;
    }
    else {
        /* Verify that the expected data structures were returned */
        if (NULL == val || PMIX_STRING != val->type ||
                           NULL == val->data.string) {
            fprintf(stderr, "[%s:%d:%lu] Failed to get job being debugged - NULL data returned\n",
                    myproc.nspace, myproc.rank, (unsigned long)pid);
            goto done;
        }
        printf("[%s:%d:%lu] PMIX_DEBUG_JOB is '%s'\n", myproc.nspace, myproc.rank,
               (unsigned long) pid, val->data.string);
        /* Save the application namespace for later */
        target_namespace = strdup(val->data.string);
        PMIX_VALUE_RELEASE(val);
    }

    printf("[%s:%d:%lu] Debugging '%s'\n", myproc.nspace, myproc.rank,
            (unsigned long)pid, target_namespace);

    /* Get my local rank so I can determine which local proc is "mine" to
     * debug */
    val = NULL;
    if (PMIX_SUCCESS != (rc = PMIx_Get(&myproc, PMIX_LOCAL_RANK, NULL, 0,
                                       &val))) {
        fprintf(stderr, "[%s:%d:%lu] Failed to get my local rank - error %s\n",
                myproc.nspace, myproc.rank, (unsigned long)pid,
                PMIx_Error_string(rc));
        goto done;
    }

    /* Verify the expected data object was returned */
    if (NULL == val) {
        fprintf(stderr,
               "[%s:%d:%lu] Failed to get my local rank - NULL data returned\n",
               myproc.nspace, myproc.rank, (unsigned long)pid);
        goto done;
    }
    if (PMIX_UINT16 != val->type) {
        fprintf(stderr,
           "[%s:%d:%lu] Failed to get my local rank - returned wrong type %s\n",
           myproc.nspace, myproc.rank, (unsigned long)pid,
           PMIx_Data_type_string(val->type));
        goto done;
    }

    /* Save the rank */
    localrank = val->data.uint16;
    PMIX_VALUE_RELEASE(val);
    printf("[%s:%d:%lu] my local rank %d\n", myproc.nspace, myproc.rank,
            (unsigned long)pid, (int)localrank);

    /* Register an event handler specifically for when the target job
     * completes */
    DEBUG_CONSTRUCT_LOCK(&myrel.lock);
    myrel.nspace = strdup(myproc.nspace);

    PMIX_LOAD_PROCID(&wildproc, target_namespace, PMIX_RANK_WILDCARD);

    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    n = 0;
    /* Pass the lock we will use to wait for notification of the
     * PMIX_ERR_JOB_TERMINATED event */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_RETURN_OBJECT, &myrel, PMIX_POINTER);
    n++;
    /* Only call me back when this specific job terminates */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_AFFECTED_PROC, &wildproc, PMIX_PROC);

    printf("[%s:%d:%lu] registering for termination of '%s'\n",
           myproc.nspace, myproc.rank, (unsigned long)pid, wildproc.nspace);

    /* Create a lock to wait for completion of the event registration
     * callback */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, info, ninfo,
                                release_fn, evhandler_reg_callbk,
                                (void*) &mylock);
    DEBUG_WAIT_THREAD(&mylock);
    PMIX_INFO_FREE(info, ninfo);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr,
                "Failed to register handler for PMIX_ERR_JOB_TERMINATED: %s\n",
                PMIx_Error_string(mylock.status));
        rc = mylock.status;
        DEBUG_DESTRUCT_LOCK(&mylock);
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);

    /* Get our local proctable - for scalability reasons, we don't want to
     * have our "root" debugger process get the proctable for everybody and
     * send it out to us. So ask the local PMIx server for the pid's of
     * our local target processes
     */
    nq = 1;
    PMIX_QUERY_CREATE(query, nq);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_LOCAL_PROC_TABLE);
    n = 0;
    ninfo = 1;
    query[0].nqual = ninfo;
    PMIX_INFO_CREATE(query[0].qualifiers, ninfo);
    /* Set the namespace to query */
    PMIX_INFO_LOAD(&query[0].qualifiers[n], PMIX_NSPACE, target_namespace,
                   PMIX_STRING);

    /* Execute the query */
    if (PMIX_SUCCESS != (rc = PMIx_Query_info(query, nq, &query_data, &query_size))) {
        fprintf(stderr, "PMIx_Query_info failed: (%d) %s\n", rc, PMIx_Error_string(rc));
        goto done;
    }

    /* Display the process table */
    printf("[%s:%d:%lu] Local proctable received for nspace '%s' has %d entries\n",
           myproc.nspace, myproc.rank, (unsigned long)pid, target_namespace,
           (int)query_data[0].value.data.darray->size);

    proctable = query_data[0].value.data.darray->array;
    for (i = 0; i < query_data[0].value.data.darray->size; i++) {
        printf("Proctable[%d], namespace %s rank %d exec %s\n", i,
               proctable[i].proc.nspace, proctable[i].proc.rank,
               basename(proctable[i].executable_name));
    }

    /* Now that we have the proctable for our local processes, this daemon can
     * interact with application processes, such as setting initial breakpoints,
     * or other setup for the debugging * session.
     * If the application was launched by the debugger, then all application
     * tasks should be suspended in PMIx_Init, usually within the application's
     * MPI_Init call.
     * Once initial setup is complete, the daemon sends a release event to the
     * application processes and those processes resume execution.
     */
    (void)strncpy(wildproc.nspace, target_namespace, PMIX_MAX_NSLEN);
    wildproc.rank = PMIX_RANK_WILDCARD;
    n = 0;
    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);

    /* Send release notification to application namespace */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_CUSTOM_RANGE, &wildproc, PMIX_PROC);
    n++;

    /* Don't send notification to default event handlers */
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL); 

    printf("[%s:%u:%lu] Sending release\n", myproc.nspace, myproc.rank,
           (unsigned long)pid);
    rc = PMIx_Notify_event(PMIX_ERR_DEBUGGER_RELEASE,
                           NULL, PMIX_RANGE_CUSTOM,
                           info, ninfo, NULL, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr,
                "[%s:%u:%lu] Sending release failed with error %s(%d)\n",
                myproc.nspace, myproc.rank, (unsigned long)pid,
                PMIx_Error_string(rc), rc);
        goto done;
    }

    /* At this point the application processes should be running under debugger
     * control. The daemons can interact further with application processes as
     * needed, or just wait for the application * termination.
     * This example just waits for application termination.
     * Note that if the application processes and daemon processes are spawned
     * by the same PMIx_Spawn call, then no PMIX_ERR_JOB_TERMINATED
     * notifications are sent since the daemons are part of the same namespace
     * and are still running.
     */
    if (0 == cospawned_namespace) {
        printf("Daemon: Waiting for application namespace %s to terminate\n",
               wildproc.nspace);
        DEBUG_WAIT_THREAD(&myrel.lock);
        printf("Daemon: Application namespace %s terminated\n", wildproc.nspace);
        fflush(NULL);
    }

  done:
    if (NULL != target_namespace) {
        free(target_namespace);
    }
    /* Call PMIx_tool_finalize to shut down the PMIx runtime */
    printf("Debugger daemon ns %s rank %d pid %lu: Finalizing\n",
           myproc.nspace, myproc.rank, (unsigned long)pid);
    if (PMIX_SUCCESS != (rc = PMIx_tool_finalize())) {
        fprintf(stderr,
               "Debugger daemon ns %s rank %d:PMIx_Finalize failed: %s\n",
               myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    } else {
        printf("Debugger daemon ns %s rank %d pid %lu:PMIx_Finalize successfully completed\n",
               myproc.nspace, myproc.rank, (unsigned long)pid);
    }
    fclose(stdout);
    fclose(stderr);
    sleep(1);

    return(0);
}
