/**
 * @file dmr.c
 * @brief DMR main functionality
 *
 * Contains the publicly accessible features of the DMR library
 */

#include "dmr.h"
#include "dmr_internal.h"
#include "dmr_backend_sender.h"

static bool dmr_was_finalized = false;
static DMRState *dmr_state = NULL;
static DMRControllerState *controller_state = NULL;

/**
 * @brief Resolve the QCUT job directory from environment variables.
 *
 * Priority order:
 * - DMR_QCUT_JOB_DIR
 * - DMR_QCUT_OUTPUT_DIR
 *
 * @return Pointer to the selected env var value, or NULL if none are set/non-empty.
 */
static const char *dmr_get_qcut_job_dir_local(void)
{
    const char *p = getenv("DMR_QCUT_JOB_DIR");
    if(p && p[0] != '\0') return p;

    p = getenv("DMR_QCUT_OUTPUT_DIR");
    if(p && p[0] != '\0') return p;

    return NULL;
}

/**
 * @brief Check whether a string is NULL or empty.
 *
 * @param s Input string (may be NULL).
 * @return Non-zero if NULL or empty, 0 otherwise.
 */
static int dmr_is_empty(const char *s) { return (!s || s[0] == '\0'); }

/**
 * @brief Map a textual label to a DQRLabel.
 *
 * Accepted values (case-insensitive):
 * - "QC", "QPU" -> DQR_LABEL_QC
 * - "HPC"       -> DQR_LABEL_HPC
 *
 * @param lab Label string (may be NULL).
 * @return Mapped DQRLabel, or DQR_LABEL_UNDECIDED if unknown/NULL.
 */
static DQRLabel dmr_map_label(const char *lab)
{
    if(!lab) return DQR_LABEL_UNDECIDED;
    if(strcasecmp(lab, "QC") == 0 || strcasecmp(lab, "QPU") == 0) return DQR_LABEL_QC;
    if(strcasecmp(lab, "HPC") == 0) return DQR_LABEL_HPC;
    return DQR_LABEL_UNDECIDED;
}

/**
 * @brief Convert a backend route enum to a short string.
 *
 * @param b Backend route.
 * @return "HPC", "QC", or "Undefined".
 */
static const char *dmr_route_to_str(DQRBackend b)
{
    switch(b)
    {
        case DQR_BACKEND_HPC: return "HPC";
        case DQR_BACKEND_QC:  return "QC";
        default:              return "Undefined";
    }
}

/**
 * @brief Find a fragment in a DQR context by fragment id.
 *
 * Performs a linear scan over the context fragments and matches by exact string
 * equality on @c frag_id.
 *
 * @param ctx DQR context (must not be NULL).
 * @param frag_id Fragment identifier (must not be NULL).
 * @return Pointer to the matching fragment, or NULL if not found/invalid args.
 */
static const DQRFragment *dmr_find_fragment_by_id(const DQRContext *ctx, const char *frag_id)
{
    if(!ctx || !frag_id) return NULL;
    size_t n = dqr_get_fragment_count(ctx);
    for(size_t i=0;i<n;i++)
    {
        const DQRFragment *f = dqr_get_fragment(ctx, i);
        if(f && strcmp(f->frag_id, frag_id) == 0) return f;
    }
    return NULL;
}

// Available to the user when reconfiguring if CHECKPOINT_RESTART=0
MPI_Comm DMR_INTERCOMM = MPI_COMM_NULL;

DMRAction dmr_init(int argc, char *argv[])
{
    // We run a lot of initial checks, so this is just to avoid repeating the error-out code
    bool error_is_fatal = true; // If we error out, we default to it being a fatal error
    char *error_msg = NULL;

    NodeInfo *node_data = NULL;

    char *program_name = argv ? argv[0] : "";

    bool is_original_execution = false;

    int mpi_was_initialized;
    int local_comm_size, local_comm_rank;

    // Worker processes receive some information that only the root initially knows
    MPI_Datatype initial_info_type = MPI_DATATYPE_NULL;

    debug_output("%s was called. argc: %d, program_name: %s\n", __func__, argc, program_name);

    MPI_Initialized(&mpi_was_initialized);

    // Check if MPI is initialized. If not, initialize it just to error out nicely
    if (!mpi_was_initialized)
    {
        MPI_Init(NULL, NULL);
        asprintf_or_null(&error_msg, "MPI was not initialized. Please call MPI_Init before dmr_init.\n");
        goto error_cleanup;
    }

    if (dmr_was_finalized)
    {
        error_is_fatal = false;
        asprintf_or_null(&error_msg, "Tried to initialize DMR after finalizing, but this is not supported. Ignoring.\n");
        goto error_cleanup;
    }

    if (!dmr_state)
    {
        init_dmr_state(&dmr_state);

        debug_output("About to duplicate the communicator\n");

        /*
         * Ensure that our own communications do not interfere
         * with the communications of the library user
         */
        MPI_Comm_dup(MPI_COMM_WORLD, &dmr_state->INTERNAL_COMM_WORLD);

        debug_output("Created communicator duplicate\n");

        MPI_Comm_size(dmr_state->INTERNAL_COMM_WORLD, &local_comm_size);
        MPI_Comm_rank(dmr_state->INTERNAL_COMM_WORLD, &local_comm_rank);

        dmr_state->local_comm_rank = local_comm_rank;
        dmr_state->local_comm_size = local_comm_size;

        if (local_comm_rank == PROC_ZERO)
        {
            dmr_state->is_root_process = true;
        }
    }

    else if (dmr_state->dmr_is_initialized)
    {
        error_is_fatal = false;
        asprintf_or_null(&error_msg, "Tried to initialize DMR, but it was already initialized. Ignoring.\n");
        goto error_cleanup;
    }

    if (strlen(program_name) == 0)
    {
        asprintf_or_null(&error_msg, "Invalid program name read from argv[0]. Aborting.\n");
        goto error_cleanup;
    }

    if (argc <= 0)
    {
        asprintf_or_null(&error_msg, "Invalid argument count provided (%d). Aborting.\n", argc);
        goto error_cleanup;
    }

    MPI_Comm_get_parent(&DMR_INTERCOMM);

    // We might be the original communicator, or we might have restarted because of C/R
    if (DMR_INTERCOMM == MPI_COMM_NULL)
    {
        int reconfig_count = dmr_get_reconfig_count();

        if (reconfig_count == -1)
        {
            asprintf_or_null(&error_msg, "Could not detect DMR state. Did you launch with the DMR wrapper?\n");
            goto error_cleanup;
        }

        else if (reconfig_count == 0)
        {
            // This defaults to false
            is_original_execution = true;
        }

        else
        {
            debug_output("Restarted with %d process%s through checkpoint restart mechanism\n", dmr_state->local_comm_size, dmr_state->local_comm_size > 1 ? "es" : "");
        }
    }

    // Identify the nodes we are running on and the processes assigned to each
    int number_of_nodes;
    bool success = identify_nodes(dmr_state, &number_of_nodes, &node_data);

    if (!success)
    {
        asprintf_or_null(&error_msg, "Memory allocation issue when detecting mapping of processes.\n");
        goto error_cleanup;
    }

    // Create datatype for initialization-time data exchange
    create_info_datatype(&initial_info_type);

    // For receiving and sending initial data
    DMRInitialInfo initial_info;

    // Set up root process' state, initialize Slurm and record necessary variables.
    if (dmr_state->is_root_process)
    {
        debug_output("World size on local communicator is %d\n", local_comm_size);

        init_controller_state(&controller_state);

        controller_state->arg_count = argc;

        // Copy any arguments into the controller state so they can be passed forward
        controller_state->arg_array = malloc_or_abort(sizeof(char *) * argc);

        for (int i = 0; i < argc; i++)
        {
            controller_state->arg_array[i] = strdup_or_abort(argv[i]);
        }

        controller_state->executable_name = strdup_or_abort(program_name);

        bool parsed_id, parsed_reconf_time;

        // Get 'master' Slurm job ID from the environment (only if previous parse succeeded)
        parsed_id = get_uint32_from_env(SLURM_JOBID_ENVAR, &controller_state->main_jobid);

        // Get last reconfiguration time (only if previous parse succeeded)
        parsed_reconf_time = parsed_id && get_double_from_env(DMR_RECONFIG_TIME_ENVAR, &controller_state->reconfig_start_last);

        // Any of the above failed
        if (!parsed_reconf_time)
        {
            const char *specific_issue = !parsed_id ? "Slurm job ID" : "reconfiguration time";

            asprintf_or_null(&error_msg, "Issue fetching %s info from environment.\n", specific_issue);
            goto error_cleanup;
        }

        slurm_init(NULL);

        job_info_msg_t *master_info_general = NULL;

        debug_output("About to load initial job (ID %" PRIu32 ")\n", controller_state->main_jobid);

        int loaded_job = slurm_load_job(&master_info_general, controller_state->main_jobid, 0);

        debug_output("Loaded initial job information\n");

        if (loaded_job != SLURM_SUCCESS || master_info_general->record_count != 1)
        {
            // Something went wrong loading the job
            if (loaded_job != SLURM_SUCCESS)
            {
                int error = slurm_get_errno();
                asprintf_or_null(&error_msg, "Got a job ID (%" PRIu32 ") for the master Slurm job but unexpectedly failed to load it: %s.\n", controller_state->main_jobid, slurm_strerror(error));
            }
            // Job info was loaded but the job array contained more than one job or was somehow empty
            else
            {
                asprintf_or_null(&error_msg, "Loaded master Slurm job (ID %" PRIu32 ") but found more/less jobs connected to it than expected (count %d).\n", controller_state->main_jobid, master_info_general->record_count);
            }

            goto error_cleanup;
        }

        slurm_job_info_t master_info_specific = master_info_general->job_array[0];

        controller_state->global_end_time = master_info_specific.end_time;
        controller_state->slurm_userid = master_info_specific.user_id;

        debug_output("About to query for and add Slurm job + process count records\n");

        NodeHashTable *node_hashtable = NULL;

        for (int i = 0; i < number_of_nodes; i++)
        {
            // Hash by IP to ensure no disagreement between MPI and SLURM
            NodeHashTable *entry = malloc_or_abort(sizeof(NodeHashTable));
            entry->node_ip = hostname_to_ip(node_data[i].node_name);
            entry->processes = node_data[i].proc_count;
            HASH_ADD_KEYPTR(hh, node_hashtable, entry->node_ip, strlen(entry->node_ip), entry);
        }

        // We have hashed this information, so we no longer need it here
        free(node_data);
        node_data = NULL;

        // Query for existing Slurm jobs and record information about them
        bool added_jobs = sync_with_slurm(controller_state, node_hashtable, is_original_execution);

        NodeHashTable *entry = NULL;
        NodeHashTable *tmp = NULL;

        // Free the resources of the hashtable, regardless of the outcome of adding jobs
        HASH_ITER(hh, node_hashtable, entry, tmp)
        {
            HASH_DEL(node_hashtable, entry);
            free(entry->node_ip);
            free(entry);
        }

        if (!added_jobs)
        {
            asprintf_or_null(&error_msg, "Something went wrong finding Slurm jobs. Aborting.\n");
            goto error_cleanup;
        }

        controller_state->new_node_count = number_of_nodes;
        debug_output("Running on a total of %d nodes over %d active jobs. Jobs to kill: %d\n", controller_state->new_node_count, controller_state->jobs_count_alive, controller_state->jobs_count_dead);

        // Set to default values
        if (jobs_can_shrink())
        {
            // This line needs to be after we have identified the node count
            int nodes_identified = controller_state->new_node_count;

            int default_shrink = get_default_nodes_in_shrink();

            if (default_shrink >= nodes_identified)
            {
                default_shrink = nodes_identified - 1;
            }

            controller_state->nodes_next_shrink = default_shrink;
            controller_state->procs_next_shrink = get_procs_removed_in_shrink(controller_state, default_shrink);

            debug_output("Able to shrink jobs. Set default nodes in shrink to %d / processes in shrink to %d\n", controller_state->nodes_next_shrink, controller_state->procs_next_shrink);
        }
        else if(dmr_get_active_expansions() > 0)
        {
            // Shrink by a single job as default
            dmr_set_jobs_next_shrink(1);
            debug_output("Cannot shrink jobs. Set default jobs in shrink to 1.\n");
        }
        else
        {
            debug_output("Cannot shrink jobs and no expansions are active.\n");
        }

        /*
         * Let the non-root processes know about setup information,
         * so that it can be queried later without additional synchronization
         */

        initial_info.int_data[0] = controller_state->new_node_count;
    }

    MPI_Bcast(&initial_info, 1, initial_info_type, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD);

    dmr_state->current_nodes = initial_info.int_data[0];

    MPI_Type_free(&initial_info_type);

    if (should_print_analytics() && dmr_state->is_root_process)
    {
        if(is_original_execution)
        {
            dmr_output("[DMR ANALYTICS],<current time>,<function>,<state>,<world size>,<node count>,<reconfiguration time>,<communication efficiency>\n");
        }

        dmr_analytics(dmr_state, controller_state, get_global_time(), -1.0, __func__, "DMR_INIT_COMPLETE", true);
    }

    dmr_state->dmr_is_initialized = true;

    if (!is_original_execution)
    {
        // Time for user to call reconfiguration logic, after which we can disconnect the intercomm and kill spare jobs if applicable
        dmr_state->action_in_progress = RESTARTED_NEED_DATA;
        return DMR_RESTART_RECONF;
    }

    // First processes to launch
    return DMR_NO_ACTION;

/*
 * Something went wrong. Clean up resources and print error.
 * Abort if necessary.
 */
error_cleanup:

    if (error_is_fatal && dmr_state)
    {
        free_dmr_state(dmr_state);
    }

    if (error_is_fatal && controller_state)
    {
        free_controller_state(controller_state);
    }

    if (local_comm_rank == PROC_ZERO && error_msg)
    {
        dmr_error(error_msg);
    }

    if (error_msg)
    {
        free(error_msg);
    }

    if (initial_info_type != MPI_DATATYPE_NULL)
    {
        MPI_Type_free(&initial_info_type);
    }

    if (node_data)
    {
        free(node_data);
    }

    if (error_is_fatal)
    {
        dmr_abort();
    }

    return DMR_NO_ACTION;
}

DMRAction dmr_check(DMRSuggestion suggested_reconfiguration)
{
    debug_output("%s was called. Suggestion: %s\n", __func__, suggestion_to_string(suggested_reconfiguration));

    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return DMR_NO_ACTION;
    }

    if (dmr_state->dmr_soft_crash)
    {
        debug_output("Ignoring %s due to soft crash.\n", __func__);
        return DMR_NO_ACTION;
    }

    if(talp_enabled())
    {
        // Start monitoring region(s) if they are not already started
        start_monitoring_regions(dmr_state);

        if(should_print_analytics())
        {
            print_talp_CE_acc(dmr_state, controller_state);
            print_talp_CE_ins(dmr_state, controller_state);
        }
    }

    // We are not currently performing any action; let the user suggest one
    if (dmr_state->action_in_progress == NONE)
    {
        debug_output("dmr_check: no action in progress.\n");

        // If an inhibitor is set, we ignore all but one out of every inhibitor_val dmr_checks
        if (dmr_state->current_reconf_step_inhibitor < dmr_state->reconf_step_inhibitor)
        {
            debug_output("dmr_check: current reconfiguration step is inhibited (%d < %d).\n", dmr_state->current_reconf_step_inhibitor, dmr_state->reconf_step_inhibitor);
            dmr_state->current_reconf_step_inhibitor++;
            return DMR_NO_ACTION;
        }
        else
        {
            dmr_state->current_reconf_step_inhibitor = 0;
        }

        DMRSuggestion original = suggested_reconfiguration;
        suggested_reconfiguration = process_if_policy(dmr_state, controller_state, suggested_reconfiguration);
        
        bool is_policy_suggestion = original != suggested_reconfiguration;

        if(is_policy_suggestion)
        {
            debug_output("Converted policy suggestion to %s\n", suggestion_to_string(suggested_reconfiguration));
        }

        // Nothing that needs to be done
        if (suggested_reconfiguration == SHOULD_STAY)
        {
            dmr_analytics(dmr_state, controller_state, get_global_time(), -1.0, __func__, "STAY_CURRENT", false);
            return DMR_NO_ACTION;
        }

        if (suggested_reconfiguration == SHOULD_SHRINK)
        {
            if (dmr_state->is_root_process)
            {
                debug_output("Shrink suggested. Removing %d processes, i.e. %d whole node(s).\n", dmr_get_procs_next_shrink(), dmr_get_nodes_next_shrink());
            }

            // Check if we can actually remove resources in the way we need to
            if (!can_remove_resources(dmr_state))
            {
                if (dmr_state->is_root_process)
                {
                    if(!is_policy_suggestion)
                    {
                        dmr_output("Shrinking in the way requested was not possible. Ignoring suggestion.\n");
                    }
                    else
                    {
                        debug_output("Silently ignoring policy-suggested shrink as it cannot be served.\n");
                    }
                }

                return DMR_NO_ACTION;
            }

            // Print analytics
            if (dmr_state->is_root_process)
            {
                controller_state->reconfig_start_curr = get_global_time(); // To send to the next iteration
                dmr_analytics(dmr_state, controller_state, controller_state->reconfig_start_curr, -1.0, __func__, "START_SHRINK", false);
            }

            dmr_state->action_in_progress = SHRINKING;

            return DMR_RECONF;
        }

        if (suggested_reconfiguration == SHOULD_EXPAND)
        {
            if (dmr_state->is_root_process)
            {
                dmr_analytics(dmr_state, controller_state, get_global_time(), -1.0, __func__, "START_EXPAND_SLURM", false);
                debug_output("Expanding. Nodes in expand: %d, procs in expand: %d\n", dmr_get_nodes_next_expand(), dmr_get_procs_next_expand());
            }

            // We need to submit a Slurm job that we can expand into
            // This action could fail, but we synchronize that later
            // at a more natural point (see below).
            expand_slurm_world(dmr_state, controller_state);

            dmr_state->action_in_progress = EXPAND_WAIT_FOR_SLURM;
        }
    }

    // From this point on, ignore any potential suggestion, because we are already taking some action

    if (dmr_state->action_in_progress == EXPAND_WAIT_FOR_SLURM)
    {
        debug_output("dmr_check: currently waiting for Slurm.\n");

        do
        {
            if (dmr_state->is_root_process)
            {
                bool ready_to_proceed = manage_pending_job(controller_state);
                bool job_successful = broadcast_expander_status(dmr_state, controller_state, ready_to_proceed);

                // Ready to proceed, i.e. the expander job started running, or somehow failed.
                if (ready_to_proceed)
                {
                    if (!job_successful)
                    {
                        dmr_state->action_in_progress = NONE;
                        return DMR_NO_ACTION;
                    }

                    controller_state->reconfig_start_curr = get_global_time(); // To send to the next iteration
                    dmr_analytics(dmr_state, controller_state, controller_state->reconfig_start_curr, -1.0, __func__, "START_EXPAND_MPI", false);

                    dmr_state->action_in_progress = EXPAND_READY_RECONFIG;
                    return DMR_RECONF;
                }
            }

            // Worker processes
            else
            {
                // Check if the controller process has determined an outcome for the expander job.
                // May or may not be blocking, depending on whether or not we enforce synchronization
                bool success = query_expand_result(dmr_state);

                if (success)
                {
                    debug_output("Worker process received OK from parent\n");

                    if (dmr_state->dmr_soft_crash)
                    {
                        dmr_state->action_in_progress = NONE;
                        return DMR_NO_ACTION;
                    }

                    dmr_state->action_in_progress = EXPAND_READY_RECONFIG;

                    return DMR_RECONF;
                }
            }

        if(requests_are_blocking())
        {
            // Cooldown to avoid overloading the Slurm controller with requests
            sleep(BLOCKING_REQ_SLEEPTIME_S);
        }

        // If resource requests are blocking, we will just check until something happens
        } while (requests_are_blocking());

        // If non-blocking resource requests, return control to the application
        return DMR_NO_ACTION;
    }

    // Finally, gracefully handle unexpected calls to dmr_check()

    char *the_expected_call;

    if (dmr_state->action_in_progress == RESTARTED_NEED_DATA)
    {
        the_expected_call = "dmr_reconfigure()";
    }
    else
    {
        the_expected_call = "dmr_finalize()";
    }

    if (dmr_state->is_root_process)
    {
        dmr_error("Called dmr_check, but expected a call to %s. Ignoring...\n", the_expected_call);
    }

    return DMR_NO_ACTION;
}

DMRAction dmr_reconfigure(void)
{
    debug_output("%s was called. Action in progress: %s\n", __func__, dmr_state ? internal_action_to_string(dmr_state->action_in_progress) : "(NULL)");

    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        init_dmr_state(&dmr_state);
        dmr_state->dmr_soft_crash = true;
        return DMR_NO_ACTION;
    }

    if (dmr_state->dmr_soft_crash)
    {
        debug_output("Ignoring %s due to soft crash.\n", __func__);
        return DMR_NO_ACTION;
    }

    // For cases where we recently expanded/shrank. The user calls reconfigure when they have completed data
    // redistribution and are ok with the prior processes disconnecting.
    if (dmr_state->action_in_progress == RESTARTED_NEED_DATA)
    {
        if (DMR_INTERCOMM != MPI_COMM_NULL)
        {
            /*
             * WARNING: if things are working, you should probably leave the ordering of freeing the
             * intercommunicator untouched. This seems to be the least glitchy way of doing things.
             */
            free_dmr_intercomm("child");
        }
        
        // Finish the shrink, if applicable
        if(dmr_state->is_root_process)
        {
            slurm_dmr_reconfigure(controller_state);
        }

        dmr_analytics(dmr_state, controller_state, get_global_time(), 1.0, __func__, "DATA_REDIST_COMPLETE", true);

        dmr_state->action_in_progress = NONE;
        return DMR_NO_ACTION;
    }

    if (dmr_state->action_in_progress == SHRINKING)
    {
        stop_and_finalize_dlb();

        if (dmr_state->is_root_process)
        {
            int hosts_to_delete = dmr_get_nodes_next_shrink();

            controller_state->new_node_count -= hosts_to_delete;

            debug_output("Deleted %d hosts from node counts. New expected node count is %d\n", hosts_to_delete, controller_state->new_node_count);
        }

        if (use_checkpoint_restart())
        {
            // Restart the DVM fully
            bool success = checkpoint_dmr_state(dmr_state, controller_state);
            if(!success)
            {
                dmr_error("Something went wrong checkpointing DMR state.\n");
            }
        }

        else
        {
            // Restart in current DVM with less processes
            spawn_mpi_world(dmr_state, controller_state);
        }

        dmr_state->action_in_progress = SHRINKING_FINALIZING;

        return DMR_REDIST_FINALIZE;
    }

    // Completely recreate the world, but this time with dmr_get_procs_next_expand() more processes
    if (dmr_state->action_in_progress == EXPAND_READY_RECONFIG)
    {
        stop_and_finalize_dlb();
        
        if (use_checkpoint_restart())
        {
            // Restart the DVM fully
            bool success = checkpoint_dmr_state(dmr_state, controller_state);
            if(!success)
            {
                dmr_error("Something went wrong checkpointing DMR state.\n");
            }
        }

        else
        {
            // Expand the DVM and the MPI world
            spawn_mpi_world(dmr_state, controller_state);
        }

        dmr_state->action_in_progress = EXPANDED_FINALIZING;

        return DMR_REDIST_FINALIZE;
    }

    // Finally, gracefully handle unexpected calls to dmr_reconfigure()

    char *the_expected_call;

    if (dmr_state->action_in_progress == NONE || dmr_state->action_in_progress == EXPAND_WAIT_FOR_SLURM)
    {
        the_expected_call = "dmr_check()";
    }
    else
    {
        the_expected_call = "dmr_finalize()";
    }

    if (dmr_state->is_root_process)
    {
        dmr_error("Called dmr_reconfigure, but expected a call to %s. Ignoring...\n", the_expected_call);
    }

    return DMR_NO_ACTION;
}

DMRAction dmr_finalize(void)
{
    debug_output("%s was called. Action in progress: %s\n", __func__, dmr_state ? internal_action_to_string(dmr_state->action_in_progress) : "(NULL)");

    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return DMR_NO_ACTION;
    }

    // Kill any expander job which we have not yet expanded onto
    if(dmr_pending_expansion())
    {
        dmr_cancel_expansion();
    }

    // Record relevant details so we do not lose them when freeing resources
    InternalAction curr_action = dmr_state->action_in_progress;
    bool is_root_process = dmr_state->is_root_process;
    int my_rank = dmr_state->local_comm_rank;

    if (is_root_process)
    {
        free_controller_state(controller_state);
        controller_state = NULL;
    }

    free_dmr_state(dmr_state);
    dmr_state = NULL;

    /*
    * WARNING: if things are working, you should probably leave the ordering of freeing the
    * intercommunicator untouched. This seems to be the least glitchy way of doing things.
    */
    free_dmr_intercomm("parent");

    // Clean up Slurm resources
    if (is_root_process)
    {
        slurm_fini();
    }

    dmr_was_finalized = true;

    if (curr_action == SHRINKING_FINALIZING || curr_action == EXPANDED_FINALIZING)
    {
        MPI_Finalize();

        debug_output("Past MPI_Finalize() on rank %d\n", my_rank);

        exit(EXIT_SUCCESS);
    }

    else if(talp_enabled())
    {
        stop_monitoring_regions();
    }

    return DMR_CLEANUP;
}

int dmr_intercomm_available(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return 0;
    }

    // In cases where shrinking does not happen through a spawn,
    // DMR_INTERCOMM will be its default value, so we will not get false positives
    return DMR_INTERCOMM != MPI_COMM_NULL;
}

int dmr_get_reconfig_count(void)
{
    // Fetch this information directly from environment. The user might want to know even before dmr_init

    uint32_t reconfig_count_uint;
    bool parse_success = get_uint32_from_env(DMR_RECONFIG_COUNT_ENVAR, &reconfig_count_uint);

    if (parse_success && reconfig_count_uint <= INT32_MAX)
    {
        return (int)reconfig_count_uint;
    }

    dmr_error("Could not read the DMR reconfiguration count (%s)\n", __func__);

    return -1;
}

int dmr_get_active_expansions(void)
{
    uint32_t expansion_count_uint;
    bool parse_success = get_uint32_from_env(EXPANSION_ENVAR, &expansion_count_uint);

    if (parse_success && expansion_count_uint <= INT32_MAX)
    {
        return (int)expansion_count_uint;
    }

    return -1;
}

int dmr_get_nodes_next_expand(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return -1;
    }

    if (!dmr_state->is_root_process)
    {
        dmr_error("Called %s from a non-root process.\n", __func__);
        return -1;
    }

    return controller_state->nodes_in_next_expand;
}

void dmr_set_nodes_next_expand(int nodes)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    if (!dmr_state->is_root_process)
    {
        dmr_error("Called %s from a non-root process.\n", __func__);
        return;
    }

    if (dmr_state->action_in_progress == EXPAND_WAIT_FOR_SLURM)
    {
        dmr_error("Called %s, but an expansion is already pending.\n", __func__);
        return;
    }

    if (nodes <= 0)
    {
        dmr_error("Invalid node count %d provided to %s.\n", nodes, __func__);
        return;
    }

    controller_state->nodes_in_next_expand = nodes;
}

int dmr_get_procs_next_expand(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return -1;
    }

    if (!dmr_state->is_root_process)
    {
        dmr_error("Called %s from a non-root process.\n", __func__);
        return -1;
    }

    int procs_in_next = controller_state->procs_in_next_expand;

    if (procs_in_next < 0)
    {
        procs_in_next = controller_state->procs_per_node * controller_state->nodes_in_next_expand;
    }

    return procs_in_next;
}

void dmr_set_procs_next_expand(int procs)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    if (!dmr_state->is_root_process)
    {
        dmr_error("Called %s from a non-root process.\n", __func__);
        return;
    }

    if (dmr_state->action_in_progress == EXPAND_WAIT_FOR_SLURM)
    {
        dmr_error("Called %s, but an expansion is already pending.\n", __func__);
        return;
    }

    if (procs <= 0)
    {
        dmr_error("Invalid process count %d provided to dmr_set_procs_next_expand.\n", procs);
        return;
    }

    controller_state->procs_per_node = -1;
    controller_state->procs_in_next_expand = procs;
}

void dmr_set_ppn_next_expand(int ppn)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    if (!dmr_state->is_root_process)
    {
        dmr_error("Called %s from a non-root process.\n", __func__);
        return;
    }

    if (dmr_state->action_in_progress == EXPAND_WAIT_FOR_SLURM)
    {
        dmr_error("Called %s but an expansion is already pending.\n", __func__);
        return;
    }

    if (ppn <= 0)
    {
        dmr_error("Invalid ppn count %d provided to dmr_set_ppn_next_expand.\n", ppn);
        return;
    }

    controller_state->procs_in_next_expand = -1;
    controller_state->procs_per_node = ppn;
}

int dmr_get_procs_next_shrink(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return -1;
    }

    if (!dmr_state->is_root_process)
    {
        dmr_error("Called %s from a non-root process.\n", __func__);
        return -1;
    }

    return controller_state->procs_next_shrink;
}

void dmr_set_procs_next_shrink(int procs)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    if (jobs_can_shrink())
    {
        if (!dmr_state->is_root_process)
        {
            dmr_error("Called %s from a non-root process.\n", __func__);
            return;
        }

        if (procs <= 0 || procs >= dmr_state->local_comm_size)
        {
            dmr_error("Invalid node count %d provided to %s.\n", procs, __func__);
            return;
        }

        // This change will affect the process count, too
        controller_state->procs_next_shrink = procs;
        controller_state->nodes_next_shrink = get_nodes_removed_in_shrink(controller_state, procs);
    }

    else
    {
        dmr_error("Called %s, but DMR is configured with fixed shrink sizes. Compile with JOBS_CAN_SHRINK=1 or use dmr_set_jobs_next_shrink instead.\n", __func__);
    }
}

int dmr_get_nodes_next_shrink(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return -1;
    }

    if (!dmr_state->is_root_process)
    {
        dmr_error("Called %s from non-root process.\n", __func__);
        return -1;
    }

    return controller_state->nodes_next_shrink;
}

void dmr_set_nodes_next_shrink(int nodes)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    if (!dmr_state->is_root_process)
    {
        dmr_error("Called %s from a non-root process.\n", __func__);
        return;
    }
    
    if (jobs_can_shrink())
    {
        if (nodes <= 0 || nodes >= dmr_state->current_nodes)
        {
            dmr_error("Invalid node count %d provided to %s.\n", nodes, __func__);
            return;
        }

        // This change will affect the process count, too
        controller_state->procs_next_shrink = get_procs_removed_in_shrink(controller_state, nodes);
        controller_state->nodes_next_shrink = nodes;
    }

    else
    {
        dmr_error("Called %s, but DMR is configured with fixed shrink sizes. Compile with JOBS_CAN_SHRINK=1 or use dmr_set_jobs_next_shrink instead.\n", __func__);
    }
}

void dmr_set_jobs_next_shrink(int jobs)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    if (!dmr_state->is_root_process)
    {
        dmr_error("Called %s from non-root process.\n", __func__);
        return;
    }

    if(jobs > dmr_get_active_expansions())
    {
        dmr_error("%s: provided job count %d is greater than the number of active expansions (%d).\n", __func__, jobs, dmr_get_active_expansions());
    }

    if(jobs <= 0)
    {
        dmr_error("%s: Provided job count %d was less than 1.\n", __func__, jobs);
    }

    internal_set_jobs_next_shrink(controller_state, jobs);
}

int dmr_get_current_node_count(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return -1;
    }

    return dmr_state->current_nodes;
}

int dmr_pending_expansion(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }

        return 0;
    }

    return dmr_state->action_in_progress == EXPAND_WAIT_FOR_SLURM || dmr_state->action_in_progress == EXPAND_READY_RECONFIG;
}

void dmr_cancel_expansion(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }

        return;
    }

    if (!dmr_pending_expansion())
    {
        return;
    }

    MPI_Barrier(dmr_state->INTERNAL_COMM_WORLD);

    if (dmr_state->is_root_process)
    {
        kill_job(controller_state->expanding_jobid);
        controller_state->expanding_jobid = DEFAULT_SLURM_JOBID;

        if (controller_state->expanding_hostlist)
        {
            hostlist_destroy_set_null(&controller_state->expanding_hostlist);
            controller_state->new_node_count = dmr_state->current_nodes;
        }
    }

    if (dmr_state->expansion_request)
    {
        free(dmr_state->expansion_request);
        dmr_state->expansion_request = NULL;
    }

    dmr_state->action_in_progress = NONE;
}

void dmr_set_reconf_step_inhibitor(int steps)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    // Barrier here to make this into a collective call
    MPI_Barrier(dmr_state->INTERNAL_COMM_WORLD);

    dmr_state->reconf_step_inhibitor = steps;
}

void dmr_set_policy_min_nodes(int nodes)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    if (nodes < 1)
    {
        dmr_error("Invalid min value %d provided to %s. Must be at least 1.\n", nodes, __func__);
        return;
    }

    // Barrier here to make this into a collective call
    MPI_Barrier(dmr_state->INTERNAL_COMM_WORLD);

    dmr_state->policy_min_nodes = nodes;

    // Ensure policy max >= min
    if(dmr_state->policy_max_nodes < nodes)
    {
        dmr_state->policy_max_nodes = nodes;
    }
}

int dmr_get_policy_min_nodes(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return 0;
    }

    return dmr_state->policy_min_nodes;
}

void dmr_set_policy_max_nodes(int nodes)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    MPI_Barrier(dmr_state->INTERNAL_COMM_WORLD);

    dmr_state->policy_max_nodes = nodes;

    // Ensure min policy nodes <= max policy nodes
    if(dmr_state->policy_min_nodes > nodes)
    {
        dmr_state->policy_min_nodes = nodes;
    }
}

int dmr_get_policy_max_nodes(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return 0;
    }

    return dmr_state->policy_max_nodes;
}

void dmr_set_policy_stride(int multiplier)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    if (multiplier < 1)
    {
        dmr_error("Invalid stride value %d provided to %s. Must be at least 1.\n", multiplier, __func__);
        return;
    }

    MPI_Barrier(dmr_state->INTERNAL_COMM_WORLD);

    dmr_state->policy_stride = multiplier;
}

int dmr_get_policy_stride(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return 0;
    }

    return dmr_state->policy_stride;
}

void dmr_set_policy_pref_nodes(int nodes)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return;
    }

    MPI_Barrier(dmr_state->INTERNAL_COMM_WORLD);

    dmr_state->policy_pref_nodes = nodes;
}

int dmr_get_policy_pref_nodes(void)
{
    bool print_error;
    if (!check_dmr_initialized(dmr_state, &print_error))
    {
        if (print_error)
        {
            dmr_error("Called %s, but DMR was not initialized. Please call dmr_init(...) first.\n", __func__);
        }
        return 0;
    }

    return dmr_state->policy_pref_nodes;
}

/**
 * @brief Run the QCUT backend worker loop on MPI_COMM_WORLD.
 *
 * Entrypoint intended for non-coordintator ranks (workers). Delegates to the backend
 * worker implementation using @c MPI_COMM_WORLD.
 */
void dmr_qcut_worker_loop(void) {
    dmr_backend_worker_loop(MPI_COMM_WORLD);
}

/**
 * @brief Send stop messages to all worker ranks and return.
 *
 * Only rank 0 sends a zero-byte message with tag @c DMR_TAG_STOP to each rank
 * in [1..size-1] on @c MPI_COMM_WORLD. Non-coordinator ranks return immediately.
 */
void dmr_qcut_shutdown_workers(void) {
    int rank = 0, size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (rank != 0) return;

    for (int r = 1; r < size; ++r) {
        MPI_Send(NULL, 0, MPI_BYTE, r, DMR_TAG_STOP, MPI_COMM_WORLD);
    }
}