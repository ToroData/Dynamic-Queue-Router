/**
 * @file dmr_core.c
 * @brief DMR core functionality
 *
 * Contains the core functions which power DMR reconfigurations
 * and setup, not including direct interactions with Slurm
 */

#include "dmr.h"
#include "dmr_internal.h"

/**
 * @brief Spawn a new communicator with a different process count.
 *
 * Create a new communicator using MPI_Comm_spawn, ensuring it inherits
 * the proper environment. This function is only used if CHECKPOINT_RESTART=0
 */
void spawn_mpi_world(DMRState *dmr_state, DMRControllerState *controller_state)
{
    // Only significant at root
    int nodes = 1;

    // Only need to populate these on the root process
    char **commands = NULL;
    char ***argvs = MPI_ARGVS_NULL;
    int *array_of_procs = NULL;
    MPI_Info *array_of_infos = NULL;

    bool expanding = dmr_state->action_in_progress == EXPAND_READY_RECONFIG;

    if (dmr_state->is_root_process)
    {
        // Already contains the new node(s)
        nodes = controller_state->new_node_count;

        commands = malloc_or_abort(sizeof(char *) * nodes);
        array_of_infos = malloc_or_abort(sizeof(MPI_Info) * nodes);
        array_of_procs = malloc_or_abort(sizeof(int) * nodes);

        int processes_in_reconfig = dmr_state->local_comm_size;

        if (expanding)
        {
            processes_in_reconfig += dmr_get_procs_next_expand();
        }
        
        else
        {
            processes_in_reconfig -= dmr_get_procs_next_shrink();
        }

        // Can just do MPI_ARGVS_NULL if the only argument is the program name
        if (controller_state->arg_count > 1)
        {
            argvs = malloc_or_abort(sizeof(char **) * nodes);

            for (int i = 0; i < nodes; i++)
            {
                // Includes argv[0] which we won't include, but instead have NULL so stay the same count
                argvs[i] = malloc_or_abort(sizeof(char *) * controller_state->arg_count);

                for (int j = 0; j < controller_state->arg_count - 1; j++)
                {
                    argvs[i][j] = controller_state->arg_array[j + 1];
                }

                argvs[i][controller_state->arg_count - 1] = NULL;
            }
        }

        int new_expansion_count, new_reconfig_count;
        get_counts_post_reconfig(dmr_state, controller_state, &new_expansion_count, &new_reconfig_count);

        char environment[] =
            ENVAR_FORMAT_INT "\n" // new_expansion_count
            ENVAR_FORMAT_INT "\n" // new_reconfig_count
            ENVAR_FORMAT_DOUBLE;  // reconfig_start_curr

        char *envar_string_full = NULL;
        asprintf_or_abort(&envar_string_full, environment,
                 EXPANSION_ENVAR, new_expansion_count,                          // A count of the number of active expansions
                 DMR_RECONFIG_COUNT_ENVAR, new_reconfig_count,                  // A count of the total number of shrinks+expands performed
                 DMR_RECONFIG_TIME_ENVAR, controller_state->reconfig_start_curr // Last time a reconfiguration occurred
        );

        debug_output("The full environment is:\n%s\n", envar_string_full);

        // Initialize values to -1, so we can detect when entries are not set later
        int const unset = -1;
        memset(array_of_procs, unset, nodes * sizeof(int));

        // Set process counts in array_of_procs according to existing structure
        int procs_added, nodes_added;
        set_existing_proc_counts(controller_state, dmr_state->current_nodes, array_of_procs, processes_in_reconfig, &procs_added, &nodes_added);

        /*
         * Case 1: we are growing and we have some unassigned processes to assign across new nodes
         * Case 2: we are shrinking and a single unassigned node now has less processes
         */
        int unassigned_procs = processes_in_reconfig - procs_added;
        int unassigned_nodes = nodes - nodes_added;
        int node_idx = 0;

        compat_hostlist_t full_hostlist = merge_all_hostlists(controller_state);

        for (int i = 0; i < nodes; i++)
        {
            commands[i] = controller_state->executable_name;

            if (array_of_procs[i] == unset)
            {
                // Distribute leftover processes evenly across remaining node(s)
                array_of_procs[i] = get_proc_count_for_node(unassigned_procs, unassigned_nodes, node_idx++);
            }

            MPI_Info_create(&array_of_infos[i]);

            char *assigned_node = slurm_hostlist_shift(full_hostlist);

            if (!assigned_node)
            {
                dmr_error("Failed to find a node off the hostlist. Unexpected behavior may follow.\n");
                break;
            }

            // For hostname with nodecount, e.g. glogin4:112
            char node_slots_format[] = "%s:%d";

            char *node_string_full = NULL;
            asprintf_or_abort(&node_string_full, node_slots_format, assigned_node, array_of_procs[i]);

            MPI_Info_set(array_of_infos[i], "PMIX_ADD_HOST", node_string_full);
            MPI_Info_set(array_of_infos[i], "PMIX_HOST", node_string_full);
            MPI_Info_set(array_of_infos[i], "PMIX_ENVAR", envar_string_full);
            MPI_Info_set(array_of_infos[i], "PMIX_MAPBY", ":OVERSUBSCRIBE");
            MPI_Info_set(array_of_infos[i], "PMIX_BINDTO", ":overload-allowed");

            debug_output("Adding host %s\n", node_string_full);

            free(node_string_full);
            free(assigned_node);
        }

        slurm_hostlist_destroy(full_hostlist);
        free(envar_string_full);

        debug_output("About to call MPI_Comm_spawn_multiple, spawning %d processes across %d nodes.\n", processes_in_reconfig, nodes);
    }

    if (!dmr_state->is_root_process)
    {
        debug_output("About to call MPI_Comm_spawn_multiple\n");
    }

    MPI_Comm_spawn_multiple(nodes, commands, argvs, array_of_procs, array_of_infos, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD, &DMR_INTERCOMM, MPI_ERRCODES_IGNORE);

    debug_output("Exit MPI_Comm_spawn_multiple call\n");

    if (dmr_state->is_root_process)
    {
        for (int i = 0; i < nodes; i++)
        {
            if (controller_state->arg_count > 1)
            {
                free(argvs[i]);
            }

            MPI_Info_free(&array_of_infos[i]);
        }

        if (controller_state->arg_count > 1)
        {
            free(argvs);
        }

        free(commands);
        free(array_of_infos);
        free(array_of_procs);
    }
}

/**
 * @brief Write DMR state to file to checkpoint it
 *
 * Save DMR's state to file so that the PRRTE DVM can be restarted. This
 * function is only used if CHECKPOINT_RESTART=1
 *
 * @returns true if successful, false otherwise
 */
bool checkpoint_dmr_state(DMRState *dmr_state, DMRControllerState *controller_state)
{
    if (dmr_state->is_root_process)
    {
        char *checkpoint_file_path = getenv(DMR_STATE_FILE_ENVAR);

        if (!checkpoint_file_path)
        {
            return false;
        }

        // The file (if existed) will have been removed already by the wrapper so we can open in append mode
        FILE *checkpoint_file = fopen(checkpoint_file_path, "a");

        if (!checkpoint_file)
        {
            return false;
        }

        int new_expansion_count, new_reconfig_count;
        get_counts_post_reconfig(dmr_state, controller_state, &new_expansion_count, &new_reconfig_count);

        /* Sample format of checkpoint file:
         * <nodes and slots>;<number of expansion jobs active>;<total number of reconfigurations>;<reconfig time>;<arg count>;<argv array>
         * Note that the actual separator is a null character \0 */

        bool done = false;
        int nodes_left = controller_state->new_node_count;
        int nodes_added = 0;

        compat_hostlist_t full_hostlist = merge_all_hostlists(controller_state);

        /* Fill in the nodes and their process count. Each job not being removed
         * is guaranteed to be filled with the same number of processes it originally had
         * (specific distribution inside the jobs could vary, but probably will not)
         */

        for (int i = 0; i < controller_state->jobs_count_alive && !done; i++)
        {
            int nodes_in_job = controller_state->jobs_info[i].host_count;

            for (int j = nodes_added; j < nodes_added + nodes_in_job; j++)
            {
                int processes = controller_state->jobs_info[i].proc_counts[i];
                char *node_name = slurm_hostlist_shift(full_hostlist);

                if (!node_name)
                {
                    dmr_error("Failed to find a node off the hostlist (checkpoint_shrink_dmr). Unexpected behavior may follow.\n");
                    break;
                }
                else
                {
                    debug_output("Node name %s assigned %d processes for checkpoint\n", node_name, processes);
                }

                if (j != 0)
                {
                    // Comma separated hostnames
                    fprintf(checkpoint_file, ",");
                }

                fprintf(checkpoint_file, "%s:%d", node_name, processes);

                free(node_name);

                nodes_added += 1;
                nodes_left -= 1;

                if (nodes_left <= 0)
                {
                    done = true;
                    break;
                }
            }
        }

        slurm_hostlist_destroy(full_hostlist);

        fputc('\0', checkpoint_file);

        // New active expansions
        fprintf(checkpoint_file, "%d", new_expansion_count);
        fputc('\0', checkpoint_file);

        // New reconfiguration count
        fprintf(checkpoint_file, "%d", new_reconfig_count);
        fputc('\0', checkpoint_file);

        // This iteration's start of reconfiguration time
        fprintf(checkpoint_file, "%f", controller_state->reconfig_start_curr);
        fputc('\0', checkpoint_file);

        // Argument count passed to dmr_init
        fprintf(checkpoint_file, "%" PRIu32, controller_state->arg_count);
        fputc('\0', checkpoint_file);

        // The argv array passed to dmr_init
        for (int i = 0; i < controller_state->arg_count; i++)
        {
            fprintf(checkpoint_file, "%s", controller_state->arg_array[i]);
            fputc('\0', checkpoint_file);
        }

        fclose(checkpoint_file);
    }

    return true;
}

/**
 * @brief Check up on progress on a running job w.r.t cooldown period and resources
 *
 * Set the time that a job started running and compare it to the cooldown period of WAIT_FOR_SLURM_S.
 * Additionally, if we launched a job to be merged, try to do so now and check that we got the correct
 * resource allocation in the main job
 *
 * @returns true if ready to proceed spawning or to error out, false otherwise
 */
static bool manage_running_job(DMRControllerState *controller_state)
{
    /*
     * A Slurm job might have changed state to RUNNING, but not be ready to accept spawns yet
     * This could happen because the node is not ready to accept incoming connections,
     * or, when altering job sizes, because our Slurm allocation has not yet been updated
     */

    if (controller_state->expand_start_run == (time_t)(-1))
    {
        time_t start_run_time = get_start_run_time(controller_state->expanding_jobid);

        if (start_run_time == (time_t)(-1))
        {
            dmr_error("Something went wrong querying the expander job for start time.\n");
            controller_state->expanding_jobid = DEFAULT_SLURM_JOBID;
            return true;
        }

        controller_state->expand_start_run = start_run_time;
        debug_output("Queried job ID %" PRIu32 " and found it started running at... %s", controller_state->expanding_jobid, ctime(&controller_state->expand_start_run));

        // We may lose information about the nodes we are adding if we are altering job sizes. Therefore, record them first
        
        controller_state->expanding_hostlist = slurm_hostlist_create(NULL);
        bool success = add_all_hosts(controller_state->expanding_hostlist, controller_state->expanding_jobid, &controller_state->new_node_count);

        if (!success)
        {
            dmr_error("Something went wrong querying nodes of expander job.\n");
            controller_state->expanding_jobid = DEFAULT_SLURM_JOBID;
            return true;
        }

        /*
         * If possible, apply a technique here which merges an expander job into the main job.
         * Do this before we have spawned onto the job
         */
        if (jobs_can_grow())
        {
            bool success = merge_expander_job(controller_state);

            if (success)
            {
                // Sanity check: did we get the new resources we wanted
                int node_count = get_job_node_count(controller_state->main_jobid);
                int expected_nodes = dmr_get_current_node_count() + dmr_get_nodes_next_expand();
                success = (node_count == expected_nodes);
            }

            if (!success)
            {
                dmr_error("Something went wrong merging expander jobs.\n");
                controller_state->expanding_jobid = DEFAULT_SLURM_JOBID;
                return true;
            }
        }
    }

    double time_run = difftime(time(NULL), controller_state->expand_start_run);

    if (time_run < WAIT_FOR_SLURM_S)
    {
        debug_output("Job has started running (since %.0fs), but waiting for it to be ready.\n", time_run);

        return false;
    }

    return true;
}

/**
 * @brief Check up on the progress of a scheduled job and readjust its end time if needed
 *
 * @returns true if an outcome for the job has been determined, false otherwidse
 */
bool manage_pending_job(DMRControllerState *controller_state)
{
    if (controller_state->expand_start_run != (time_t)(-1))
    {
        return manage_running_job(controller_state);
    }

    if (controller_state->expanding_jobid != DEFAULT_SLURM_JOBID)
    {
        enum job_states job_state;

        debug_output("About to read expander job (ID %" PRIu32 ") state...\n", controller_state->expanding_jobid);

        get_job_state(controller_state->expanding_jobid, &job_state);

        debug_output("Read expander job state: %d...\n", job_state);
        
        if ((job_state & JOB_STATE_BASE) == JOB_PENDING) 
        {
            // Adjust the pending job's completion time
            // Then we're done for now.
            return !adjust_pending_job_time(controller_state);
        }

        else if (job_state & JOB_CONFIGURING)
        {
            // Do not attempt to touch the job while configuring
            return false;
        }

        else if ((job_state & JOB_STATE_BASE) == JOB_RUNNING)
        {
            // Expander job has started running; handled by manage_running_job from now on
            return manage_running_job(controller_state);
        }

        else
        {
            dmr_error("Found expansion job in unexpected state.\n");
            controller_state->expanding_jobid = DEFAULT_SLURM_JOBID;
            return true;
        }
    }

    // There is no expander job ID, report this result
    return true;
}

/**
 * @brief Broadcast an outcome of expanding the Slurm world
 *
 * @returns true if the outcome was a success, otherwise false
 */
bool broadcast_expander_status(DMRState *dmr_state, DMRControllerState *controller_state, bool ready_to_proceed)
{
    if (!ready_to_proceed)
    {
        /*
         * We are not ready to proceed, but we may still want to ensure that we operate in lockstep with
         * the other processes. Therefore, broadcast the fact that we are not yet ready
         */

        if (ENFORCE_SYNCHRONIZATION)
        {
            int expand_result_int = (int)EXPAND_PENDING;
            MPI_Bcast(&expand_result_int, 1, MPI_INT, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD);
        }

        return false;
    }

    if (controller_state->expanding_jobid == DEFAULT_SLURM_JOBID)
    {
        dmr_error("Errors have occurred which will now disable all DMR functionality.\n");
        dmr_state->expansion_status = EXPAND_FAILED;
    }

    else
    {
        dmr_state->expansion_status = EXPAND_SUCCESS;
    }

    debug_output("About to do %s\n", ENFORCE_SYNCHRONIZATION ? "MPI_Bcast" : "MPI_Ibcast");

    int expansion_status_int = (int)dmr_state->expansion_status;

    if (!ENFORCE_SYNCHRONIZATION)
    {
        MPI_Request the_request;

        MPI_Ibcast(&expansion_status_int, 1, MPI_INT, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD, &the_request);

        // Seems a bit pointless, but we have to match the other Ibcast call with an Ibcast
        MPI_Wait(&the_request, MPI_STATUS_IGNORE);
    }
    else
    {
        MPI_Bcast(&expansion_status_int, 1, MPI_INT, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD);
    }

    debug_output("Returned from %s\n", ENFORCE_SYNCHRONIZATION ? "MPI_Bcast" : "MPI_Ibcast");

    if (dmr_state->expansion_status != EXPAND_SUCCESS)
    {
        dmr_state->dmr_soft_crash = true;
        return false;
    }

    return true;
}

/**
 * @brief Get the results of a pending expansion if they are available
 *
 * @returns An ExpandResult representing the status of the expansion
 */
static ExpandResult get_expand_result_async(DMRState *dmr_state)
{
    ExpandResult result = EXPAND_PENDING;

    // We want to know when the expander jobs are ready, but if they are not we can still continue computation
    if (!dmr_state->expansion_request)
    {
        dmr_state->expansion_request = malloc_or_abort(sizeof(MPI_Request));
        MPI_Ibcast(&dmr_state->expansion_status, 1, MPI_C_BOOL, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD, dmr_state->expansion_request);
    }

    int received;
    MPI_Test(dmr_state->expansion_request, &received, MPI_STATUS_IGNORE);

    debug_output("Did MPI_Test. Received? %s\n", received ? "T" : "F");

    // We have received the results of the expansion
    if (received)
    {
        result = (ExpandResult)dmr_state->expansion_status;

        if (result != EXPAND_SUCCESS && result != EXPAND_FAILED)
        {
            dmr_error("Received unexpected result in get_expand_result_async\n");
        }

        free(dmr_state->expansion_request);
        dmr_state->expansion_request = NULL;
    }

    return result;
}

/**
 * @brief Get the status of a pending expansion in a blocking way
 *
 * @returns An ExpandResult representing the status of the expansion
 */
static ExpandResult get_expand_result_sync(DMRState *dmr_state)
{
    int result_int;
    MPI_Bcast(&result_int, 1, MPI_INT, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD);

    return (ExpandResult)result_int;
}

/**
 * @brief Check if the request to expand the Slurm world has reached an outcome
 *
 * @returns true when there is an outcome to report, otherwise false
 */
bool query_expand_result(DMRState *dmr_state)
{
    ExpandResult result;

    if (ENFORCE_SYNCHRONIZATION)
    {
        result = get_expand_result_sync(dmr_state);
    }

    else
    {
        result = get_expand_result_async(dmr_state);
    }

    if (result == EXPAND_FAILED)
    {
        dmr_state->dmr_soft_crash = true;
    }

    // Otherwise, still pending
    return result == EXPAND_SUCCESS;
}

/**
 * @brief Identify the nodes DMR is running on along with the process counts.
 *
 * Identify nodes and process counts. Every process participates in the identification,
 * but only the root process gets the information.
 */
bool identify_nodes(DMRState *dmr_state, int *number_of_nodes, NodeInfo **node_data)
{
    MPI_Comm per_node_comm;

    // Make one communicator per node
    MPI_Comm_split_type(dmr_state->INTERNAL_COMM_WORLD, MPI_COMM_TYPE_SHARED, dmr_state->local_comm_rank, MPI_INFO_NULL, &per_node_comm);

    int node_rank, node_size;
    MPI_Comm_rank(per_node_comm, &node_rank);
    MPI_Comm_size(per_node_comm, &node_size);

    MPI_Comm_free(&per_node_comm);

    int color = node_rank == 0 ? 0 : MPI_UNDEFINED;

    MPI_Comm across_node_comm = MPI_COMM_NULL;

    // Make a communicator with the rank-0s of every node w.r.t the communicators we just created
    MPI_Comm_split(dmr_state->INTERNAL_COMM_WORLD, color, dmr_state->local_comm_rank, &across_node_comm);

    if (across_node_comm != MPI_COMM_NULL)
    {
        int len_ignore;
        NodeInfo my_info;

        MPI_Get_processor_name(my_info.node_name, &len_ignore);

        MPI_Comm_size(across_node_comm, number_of_nodes);

        MPI_Datatype node_datatype;
        create_nodeinfo_datatype(&node_datatype);

        if (dmr_state->is_root_process)
        {
            *node_data = malloc_or_abort(sizeof(NodeInfo) * *number_of_nodes);

            if (!*node_data)
            {
                debug_output("Memory allocation issue on current node\n");
                return false;
            }
        }

        my_info.proc_count = node_size;

        MPI_Gather(&my_info, 1, node_datatype, *node_data, 1, node_datatype, 0, across_node_comm);

        MPI_Comm_free(&across_node_comm);
    }

    return true;
}


