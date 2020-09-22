/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prte_config.h"
#include "constants.h"

#include "src/util/output.h"
#include "src/util/argv.h"
#include "src/mca/base/prte_mca_base_framework.h"

#include "src/mca/ess/ess.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/runtime.h"
#include "src/util/listener.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

int prte_finalize(void)
{
    int rc;
    uint32_t key;
    prte_job_t *jdata = NULL, *child_jdata = NULL;
    void *elt = NULL;

    --prte_initialized;
    if (0 != prte_initialized) {
        /* check for mismatched calls */
        if (0 > prte_initialized) {
            prte_output(0, "%s MISMATCHED CALLS TO PRTE FINALIZE",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        }
        return PRTE_ERROR;
    }

    /* protect against multiple calls */
    if (prte_atomic_trylock(&prte_finalize_lock)) {
        return PRTE_SUCCESS;
    }

    /* flag that we are finalizing */
    prte_finalizing = true;

    /* stop listening for connections - will
     * be ignored if no listeners were registered */
    prte_stop_listening();

    /* release the cache */
    PRTE_RELEASE(prte_cache);

    prte_display_prte_job_data("Finalize");

    /* Release the job hash table
     *
     * First we need to iterate through an release all of the children.
     * Since we cannot know the order in which items are removed from the hash
     * table, then if we do not do this then we may try to free a child job
     * before the parent which would trigger an assert in the list item
     * destructor which checks to make sure this item was removed from the list
     * before being destructed.
     *
     * Second (and not with the first loop) we can then destruct the
     * jobs in the hash table knowing that they are definitely not on a
     * children list.
     */
    PRTE_HASH_TABLE_FOREACH(key, uint32, jdata, prte_job_data) {
        if (NULL != jdata) {
            // Remove all children from the list
            // We do not want to destruct this list here since that occurs in the
            // prte_job_t destructor - which will happen in the next loop.
            PRTE_LIST_FOREACH(child_jdata, &jdata->children, prte_job_t) {
                prte_list_remove_item(&jdata->children, &child_jdata->super);
            }
        }
    }

    jdata = NULL;
    do {
        rc = prte_hash_table_get_next_key_uint32(prte_job_data, &key, (void**)&jdata, NULL, &elt);
        if (PRTE_SUCCESS == rc) {
            prte_hash_table_remove_value_uint32(prte_job_data, key);
            if (NULL != jdata) {
                PRTE_RELEASE(jdata);
            }
        }
    } while (PRTE_SUCCESS == rc);
    PRTE_RELEASE(prte_job_data);

    if (prte_do_not_launch) {
        exit(0);
    }

{
    prte_pointer_array_t * array = prte_node_topologies;
    int i;
    if( array->number_free != array->size ) {
        prte_mutex_lock(&array->lock);
        array->lowest_free = 0;
        array->number_free = array->size;
        for(i=0; i<array->size; i++) {
            if(NULL != array->addr[i]) {
                prte_topology_t * topo = (prte_topology_t *)array->addr[i];
                topo->topo = NULL;
                PRTE_RELEASE(topo);
            }
            array->addr[i] = NULL;
        }
        prte_mutex_unlock(&array->lock);
    }
}
    PRTE_RELEASE(prte_node_topologies);

{
    prte_pointer_array_t * array = prte_node_pool;
    int i;
    prte_node_t *node;
    if( array->number_free != array->size ) {
        prte_mutex_lock(&array->lock);
        array->lowest_free = 0;
        array->number_free = array->size;
        for(i=0; i<array->size; i++) {
            if(NULL != array->addr[i]) {
                node= (prte_node_t*)array->addr[i];
                if (NULL != node) {
                    if (NULL != node->daemon) {
                        PRTE_RELEASE(node->daemon);
                    }
                    PRTE_RELEASE(node);
                }
            }
            array->addr[i] = NULL;
        }
        prte_mutex_unlock(&array->lock);
    }
}
    PRTE_RELEASE(prte_node_pool);
    /* call the finalize function for this environment */
    if (PRTE_SUCCESS != (rc = prte_ess.finalize())) {
        return rc;
    }

    /* finalize schizo */
    prte_schizo.finalize();

    /* Close the general debug stream */
    prte_output_close(prte_debug_output);

    if (NULL != prte_fork_agent) {
        prte_argv_free(prte_fork_agent);
    }

    /* finalize the class/object system */
    prte_class_finalize();

    free (prte_process_info.nodename);
    prte_process_info.nodename = NULL;

    return PRTE_SUCCESS;
}
