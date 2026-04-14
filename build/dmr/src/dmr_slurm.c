/**
 * @file dmr_slurm.c
 * @brief DMR internal logic relating to Slurm 
 *
 * Contains logic used in internal functions of DMR relating
 * to Slurm and the Slurm API. Not intended for external use.
 */

#include "dmr.h"
#include "dmr_internal.h"

#if SLURM_VERSION_NUMBER < SLURM_VERSION_NUM(20, 11, 0)

/**
 * @brief Dummy function for slurm_init for compiling with Slurm versions that do not have it
 */
void slurm_init(const char *conf)
{
    (void)conf; // For the compiler to not complain
}

/**
 * @brief Dummy function for slurm_fini for compiling with Slurm versions that do not have it
 */
void slurm_fini(void)
{
}

#endif

/**
 * @brief Get a Slurm partition from environment
 *
 * Checks DMR_SLURM_JOB_PARTITION, then SLURM_JOB_PARTITION
 *
 * @return Pointer to partition string or empty string if not found
 */
static char *get_slurm_partition(void)
{
    // See if there is an override active
    char *partition = getenv(DMR_SLURM_JOB_PARTITION);

    // Else try the Slurm default environment variable
    if (!partition || *partition == '\0') 
    {
        partition = getenv(SLURM_PARTITION_ENVAR);
    }

    if (!partition) 
    {
        partition = "";
    }

    return partition;
}

/**
 * @brief Get a Slurm QoS from environment
 *
 * Checks DMR_SLURM_JOB_QOS, then SLURM_JOB_QOS
 *
 * @return Pointer to QoS string or empty string if not found
 */
static char *get_slurm_qos(void)
{
    // See if there is an override active
    char *qos = getenv(DMR_SLURM_JOB_QOS);

    // Else try the Slurm default environment variable
    if (!qos || *qos == '\0')
    {
        qos = getenv(SLURM_QOS_ENVAR);
    }

    if (!qos) 
    {
        qos = "";
    }

    return qos;
}

/**
 * @brief Get a Slurm account from environment.
 *
 * Checks DMR_SLURM_JOB_ACCOUNT, then SLURM_JOB_ACCOUNT
 *
 * @return Pointer to account string or empty string if not found
 */
static char *get_slurm_account(void)
{
    // See if there is an override active
    char *account = getenv(DMR_SLURM_JOB_ACCOUNT);

    // Else try the Slurm default environment variable
    if (!account || *account == '\0') 
    {
        account = getenv(SLURM_ACCOUNT_ENVAR);
    }

    if (!account) 
    {
        account = "";
    }

    return account;
}

/**
 * @brief Get a SLURM ranged hostlist string by dynamically allocated buffer
 *
 * Helper function for cross-version SLURM compatibility to create a buffer for and
 * allocate a nodelist string from a hostlist.
 *
 * @returns Dynamically allocated hostlist string which must be freed, or NULL in failure cases
 */
static char *ranged_hostlist_string(compat_hostlist_t hostlist)
{
    int const failed = -1;
    size_t const initial_bufsize = 1024; // Arbitrary size buffer to start with
    size_t const max_bufsize = initial_bufsize * 100;

    size_t curr_bufsize = initial_bufsize;

    char *hostlist_string = malloc(curr_bufsize);

    if(!hostlist_string)
    {
        return NULL;
    }

    ssize_t written = slurm_hostlist_ranged_string(hostlist, initial_bufsize, hostlist_string);

    while (written == failed && curr_bufsize < max_bufsize) 
    {
        curr_bufsize *= 2; // Increase buffer size
        char *tmp_string = realloc(hostlist_string, curr_bufsize);

        if(!tmp_string)
        {
            break;
        }

        hostlist_string = tmp_string;
        written = slurm_hostlist_ranged_string(hostlist, curr_bufsize, hostlist_string);
    }

    if(written == failed)
    {
        free(hostlist_string);
        return NULL;
    }

    return hostlist_string;
}

/**
 * @brief Use the Slurm API to load a job by its job_id and get its start time
 *
 * @returns The time_t the given job started running, or (time_t)(-1) if failed
 */
time_t get_start_run_time(uint32_t job_id)
{
    time_t start_run_time = (time_t)(-1);
    job_info_msg_t *job_info_general = NULL;

    int load_result = slurm_load_job(&job_info_general, job_id, 0);

    if (load_result == SLURM_SUCCESS && job_info_general->record_count == 1)
    {
        start_run_time = job_info_general->job_array[0].start_time;
    }

    if (job_info_general)
    {
        slurm_free_job_info_msg(job_info_general);
    }

    return start_run_time;
}

/**
 * @brief Add information about hosts and process counts into a DMRSlurmJobInfo
 *
 * @returns true when completed successfully, false otherwise
 */
static bool add_hosts_with_counts(char *slurm_host_string, NodeHashTable *node_hashtable, DMRSlurmJobInfo *jobs_info, uint32_t *real_count)
{
    compat_hostlist_t temp_hostlist = slurm_hostlist_create(slurm_host_string);

    char *host = slurm_hostlist_shift(temp_hostlist);

    bool success = true;

    uint32_t idx = 0;

    int total_procs = 0;

    while (host && success)
    {
        // Avoid inconsistensies between Slurm and MPI names
        char *host_ip = hostname_to_ip(host);

        NodeHashTable *match = NULL;
        HASH_FIND_STR(node_hashtable, host_ip, match);

        free(host_ip);

        if(!match)
        {
            debug_output("Host %s had no processes / no entry in hash table\n", host);

            host = slurm_hostlist_shift(temp_hostlist);

            // Could be a node due to shrink; ignore
            continue;
        }

        success = slurm_hostlist_push_host(jobs_info->hostlist, host);

        if(!success)
        {
            break;
        }

        jobs_info->proc_counts[idx++] = match->processes;
        total_procs+=match->processes;

        free(host);

        host = slurm_hostlist_shift(temp_hostlist);
    }

    if(host)
    {
        free(host);
    }

    slurm_hostlist_destroy(temp_hostlist);

    jobs_info->total_procs = total_procs;

    *real_count = idx;

    return success;
}

/**
 * @brief Add the nodes from a Slurm job by ID into a Slurm hostlist datatype
 *
 * @returns true when completed successfully, false otherwise
 */
bool add_all_hosts(compat_hostlist_t the_hostlist, uint32_t job_id, int *list_count)
{
    if(!list_count)
    {
        return false;
    }

    job_info_msg_t *job_info_general = NULL;
    int load_result = slurm_load_job(&job_info_general, job_id, 0);
    int num_inserted = 0;

    if (load_result == SLURM_SUCCESS && job_info_general->record_count == 1)
    {
        char *hostlist_string = job_info_general->job_array[0].nodes;
        num_inserted = slurm_hostlist_push(the_hostlist, hostlist_string);
    }

    if (job_info_general)
    {
        slurm_free_job_info_msg(job_info_general);
    }

    if(num_inserted <= 0)
    {
        return false;
    }

    (*list_count)+=num_inserted;

    return true;
}

/**
 * @brief Move all the hosts from from_hostlist to to_hostlist, leaving from_hostlist empty
 *
 * @returns true when completed successfully, false otherwise
 */
static bool move_hosts(compat_hostlist_t to_hostlist, compat_hostlist_t from_hostlist)
{
    if(!to_hostlist || !from_hostlist)
    {
        dmr_error("to_hostlist null ? %s and/or from_hostlist null %s (%s)\n", to_hostlist ? "F" : "T", from_hostlist ? "F" : "T", __func__);
        return false;
    }

    char *host = slurm_hostlist_shift(from_hostlist);

    bool success = true;

    while (host && success)
    {
        success = slurm_hostlist_push_host(to_hostlist, host);

        free(host);
        host = slurm_hostlist_shift(from_hostlist);
    }

    if(host)
    {
        free(host);
    }

    return success;
}

/**
 * @brief Function to compare two Slurm jobs by their job ID; used for qsort
 */
static int compare_jobs(const void *a, const void *b)
{
    slurm_job_info_t *a_job = (slurm_job_info_t *)a;
    slurm_job_info_t *b_job = (slurm_job_info_t *)b;

    if (a_job->job_id < b_job->job_id)
    {
        return -1;
    }

    if (a_job->job_id > b_job->job_id)
    {
        return 1;
    }

    return 0;
}

/**
 * @brief Query for and add Slurm jobs and nodes belonging to the current DMR execution
 *
 * @returns true when completed successfully, false otherwise
 */
bool sync_with_slurm(DMRControllerState *controller_state, NodeHashTable *node_hashtable, bool original_execution)
{
    char *prefix_to_match;
    asprintf_or_abort(&prefix_to_match, EXPAND_JOBNAME_MAIN, controller_state->main_jobid);
    int prefix_len = strlen(prefix_to_match);

    int idx = 0;

    job_info_msg_t *user_jobs;
    int result = slurm_load_job_user(&user_jobs, controller_state->slurm_userid, 0);

    int expected_jobs = dmr_get_active_expansions()+1;

    bool success = true;

    if (result == SLURM_SUCCESS && user_jobs->record_count > 0)
    {
        /*
        * Ensure we are working from low job IDs to high job IDs
        * The results are probably going to be sorted either way, but the
        * Slurm API documentation does not say this explicitly.
        */
        qsort(user_jobs->job_array, user_jobs->record_count, sizeof(slurm_job_info_t), compare_jobs);

        // All the jobs the user has submitted; including potentially non-DMR ones
        for (uint32_t i = 0; i < user_jobs->record_count; i++)
        {
            // Found a Slurm job from current user; need to make sure it's relevant to our run
            slurm_job_info_t current = user_jobs->job_array[i];

            if((current.job_state & JOB_STATE_BASE) != JOB_RUNNING)
            {
                continue;
            }

            if (strncmp(current.name, prefix_to_match, prefix_len) == 0 || current.job_id == controller_state->main_jobid)
            {   
                if (idx >= MAX_EXPANSIONS - 1)
                {
                    dmr_error("Exceeded MAX_EXPANSIONS count. It can be increased in dmr_internal.h.\n");
                    success = false;
                    break;
                }

                int jobs_index = controller_state->jobs_count_alive + controller_state->jobs_count_dead;
                DMRSlurmJobInfo *curr = &controller_state->jobs_info[jobs_index];
                curr->job_id = current.job_id;

                // Further jobs are due to terminate due to shrinking. This assumes we're working with a sorted array.
                if (idx >= expected_jobs)
                {
                    // Mark for termination; may still require some data transfer to complete before this is done
                    curr->should_kill = true;
                    controller_state->jobs_count_dead++;
                    continue;
                }

                controller_state->jobs_count_alive++;

                curr->proc_counts = malloc_or_abort(sizeof(int) *current.num_nodes);
                curr->total_procs = 0;
                curr->hostlist = slurm_hostlist_create(NULL);

                uint32_t mpi_host_count;
                success = add_hosts_with_counts(current.nodes, node_hashtable, curr, &mpi_host_count);

                if(!success)
                {
                    dmr_error("Something went wrong adding hosts from a loaded Slurm job\n");
                    break;
                }

                /*
                * Could have some nodes due to shrink that can still be seen in the Slurm job but not in our MPI world
                * The user could have launched with more nodes than they are using, in which case we will not touch them yet
                * (attempting to do so might kill the whole execution as the DVM is launched on that node)
                */
                if(current.num_nodes > mpi_host_count && !original_execution && jobs_can_shrink())
                {
                    curr->should_shrink = true;
                }

                curr->host_count = mpi_host_count;

                idx++;

                if(curr->total_procs == 0 || mpi_host_count == 0)
                {
                    dmr_error("Unexpectedly found a job with %d processes and %d hosts.\n", curr->total_procs, mpi_host_count);
                    success = false;
                    break;
                }

                debug_output("Found and added a relevant job, ID is %" PRIu32 ", host count: %d, proc count: %d.\n", current.job_id, mpi_host_count, curr->total_procs);
            }
        }
    }

    if (success && idx != expected_jobs)
    {
        dmr_error("Expected to find %d active jobs, but found %d.\n", dmr_get_active_expansions()+1, idx);
        success = false;
    }

    if (user_jobs)
    {
        slurm_free_job_info_msg(user_jobs);
    }

    free(prefix_to_match);
    return success;
}

/**
 * @brief Use the Slurm API to submit a batch job given a job_desc_msg_t
 *
 * @returns uint32_t containing the job ID if successful, otherwise DEFAULT_SLURM_JOBID
 */
static uint32_t submit_batch_job(job_desc_msg_t *job_description)
{
    submit_response_msg_t *response = NULL;

    int submit_result = slurm_submit_batch_job(job_description, &response);

    int return_val = DEFAULT_SLURM_JOBID;

    if (submit_result == SLURM_SUCCESS)
    {
        return_val = response->job_id;
    }

    if (response)
    {
        slurm_free_submit_response_response_msg(response);
    }

    return return_val;
}

/**
 * @brief Free the resources allocated by fill_job_desc
 */
static void cleanup_job_desc(job_desc_msg_t *job_desc)
{
    free(job_desc->name);
    free(job_desc->work_dir);
    free(job_desc->environment);
    free(job_desc->script);
}

/**
 * @brief Fill a Slurm API job_desc_msg_t with the data needed to launch an expander job
 */
static void fill_job_desc(job_desc_msg_t *job_desc, time_t end_time, uint32_t original_jobid, int job_count)
{
    // No need to set any of the unused fields as this is done internally by Slurm with this call
    slurm_init_job_desc_msg(job_desc);

    char working_dir[FILE_PATH_MAX];
    getcwd(working_dir, FILE_PATH_MAX);

    int time_left_int = get_minutes_left(end_time);
    uint32_t time_left_uint;

    if (time_left_int > 0)
    {
        time_left_uint = (uint32_t)time_left_int;
    }
    else
    {
        time_left_uint = 1;
    }

    // Expander jobname includes original job ID for identification purposes
    char *job_name;
    char *job_format = EXPAND_JOBNAME_MAIN EXPAND_JOBNAME_SUFFIX;
    asprintf_or_abort(&job_name, job_format, original_jobid, job_count);

    /*
    * The job script queries for the original job until it is no longer in RUNNING state
    * If it did not loop and sleep, it would terminate immediately, and if it did not
    * query for the original job, it would end up unnecessarily idling after its termination.
    * 
    * This script originally used squeue to check if a job was visible to it, but due to some
    * edge cases which caused premature termination, it now uses scontrol to check state explicitly.
    */
    char *script_format =
    "#!/bin/bash\n"
    "while true; do\n"
    "  jobinfo=$(%sscontrol show job %" PRIu32 " 2>/dev/null)\n"
    "  jobstate=$(echo \"$jobinfo\" | awk -F= '/JobState=/ {print $2; exit}' | awk '{print $1}')\n"
    "  [[ -z \"$jobinfo\" || \"$jobstate\" == RUNNING ]] && sleep 15 || break\n"
    "done\n";

    char *script;
    
    char *squeue_prefix;
    bool empty_prefix = CUSTOM_SLURM_BIN_PREFIX[0] == '\0';

    if(!empty_prefix)
    {
        // We have a custom prefix path. Append a slash to it to find squeue
        asprintf_or_abort(&squeue_prefix, "%s/", CUSTOM_SLURM_BIN_PREFIX);
        asprintf_or_abort(&script, script_format, squeue_prefix, original_jobid);
        free(squeue_prefix);
    }
    else
    {
        // Prefix is an empty string as squeue is presumably on PATH
        asprintf_or_abort(&script, script_format, "", original_jobid);
    }

    debug_output("Script is \"%s\"\n", script);

    job_desc->name = job_name;
    job_desc->account = get_slurm_account();
    job_desc->qos = get_slurm_qos();
    job_desc->partition = get_slurm_partition();
    job_desc->script = script;
    job_desc->shared = 0;
    job_desc->std_out = EXPAND_JOB_OUTPUT_PATH;
    job_desc->min_nodes = dmr_get_nodes_next_expand();
    job_desc->max_nodes = job_desc->min_nodes;
    job_desc->time_limit = time_left_uint;
    job_desc->work_dir = strdup_or_abort(working_dir);
    job_desc->env_size = 1;
    job_desc->user_id = getuid(); // Needed for Slurm API backwards compatibility

    if (job_desc->shared)
    {
        debug_output("NOTE: Running in shared node mode.\n");
    }

    char **env = malloc_or_abort(sizeof(char *));
    // The Slurm API seems to require that at least one environment variable be present
    env[0] = "DMR_DUMMY_ENV_VAR=true";
    job_desc->environment = env;
}

/**
 * @brief Use the Slurm API get the state of a job with the given job_id
 *
 * The job state will be filled into state, or the state will be JOB_END if failed
 */
void get_job_state(uint32_t job_id, enum job_states *state)
{
    job_info_msg_t *job_info_general = NULL;
    int load_result = slurm_load_job(&job_info_general, job_id, 0);

    if (load_result == SLURM_SUCCESS && job_info_general->record_count == 1)
    {
        *state = job_info_general->job_array[0].job_state;
    }

    else
    {
        *state = JOB_END;
    }

    if (job_info_general)
    {
        slurm_free_job_info_msg(job_info_general);
    }
}

/**
 * @brief Use the Slurm API to load a job by the given job_id, then kill it
 *
 * @returns true when completed successfully, false otherwise
 */
bool kill_job(uint32_t job_id)
{
    int kill_result = slurm_kill_job(job_id, SIGKILL, KILL_FULL_JOB);
    return kill_result == SLURM_SUCCESS;
}

/**
 * @brief Use the Slurm API to adjust the end time of a pending job with id job_id to match up with global_end_time
 *
 * @returns true if adjusted successfully, otherwise false
 */
static bool adjust_job_end_time(time_t global_end_time, uint32_t job_id)
{
    int minutes_to_end = get_minutes_left(global_end_time);

    if (minutes_to_end <= 0)
    {
        return kill_job(job_id);
    }

    job_desc_msg_t job_update;
    slurm_init_job_desc_msg(&job_update);
    job_update.job_id = job_id;
    job_update.time_limit = (uint32_t)minutes_to_end;

    int stat = slurm_update_job(&job_update);

    return stat == SLURM_SUCCESS;
}

/**
 * @brief Set the number of nodes of a running job
 *
 * The count must be a number less than the current number of nodes.
 *
 * @returns true if resized succesfully, otherwise false
 */
static bool force_set_job_nodes(uint32_t job_id, uint32_t new_nodes)
{
    job_desc_msg_t job_update;
    slurm_init_job_desc_msg(&job_update);
    job_update.job_id = job_id;
    job_update.min_nodes = new_nodes;

    return slurm_update_job(&job_update) == SLURM_SUCCESS;
}

/**
 * @brief Set the specific nodes of a running job
 *
 * The count must be a number less than the current number of nodes.
 *
 * @returns true if resized succesfully, otherwise false
 */
static bool force_set_job_nodelist(uint32_t job_id, uint32_t new_count, compat_hostlist_t new_nodes)
{
    char *expanded_nodes = ranged_hostlist_string(new_nodes);

    if(!expanded_nodes)
    {
        dmr_error("Failed to get string with required nodelist in shrink. Trying anyway without the nodelist...\n");
        return force_set_job_nodes(job_id, new_count);
    }
    
    job_desc_msg_t job_update;
    slurm_init_job_desc_msg(&job_update);
    job_update.job_id = job_id;
    job_update.min_nodes = new_count;
    job_update.req_nodes = expanded_nodes;

    int stat = slurm_update_job(&job_update);

    free(expanded_nodes);

    return stat == SLURM_SUCCESS;
}

/**
 * @brief Allocate a Slurm job to be merged into the job at the specified job id
 *
 * This method requires a legacy Slurm version to succeed, though submission will work in any case
 *
 * @returns The job ID of the submitted merger job, or DEFAULT_SLURM_JOBID if failed
 */
static uint32_t alloc_expand_merger_job(uint32_t main_job_id, int new_nodes)
{
    job_desc_msg_t expander_job;
    slurm_init_job_desc_msg(&expander_job);
    asprintf_or_abort(&expander_job.name, "%" PRIu32 "_RESIZER", main_job_id); // Arbitrary name of expander job
    asprintf_or_abort(&expander_job.dependency, "expand:%" PRIu32, main_job_id); // This tells Slurm we want to merge jobs
    expander_job.user_id = getuid();
    expander_job.min_nodes = new_nodes;
    expander_job.max_nodes = new_nodes;

    debug_output("alloc_expand_merger_job: allocating with %d nodes\n", new_nodes);

    resource_allocation_response_msg_t *slurm_alloc_msg_ptr = NULL;
    int stat = slurm_allocate_resources(&expander_job, &slurm_alloc_msg_ptr);

    uint32_t expander_job_id = DEFAULT_SLURM_JOBID;

    if(stat == SLURM_SUCCESS)
    {
        expander_job_id = slurm_alloc_msg_ptr->job_id;
    }

    free(expander_job.dependency);
    free(expander_job.name);

    if(slurm_alloc_msg_ptr)
    {
        slurm_free_resource_allocation_response_msg(slurm_alloc_msg_ptr);
    }

    return expander_job_id;
}

/**
 * @brief Load a job and query how many nodes it is currently running on
 *
 * @returns The node count returned from Slurm for the specified job ID
 */
int get_job_node_count(uint32_t job_id)
{
    int nodes = -1;
    job_info_msg_t *job_info_general = NULL;
    int load_result = slurm_load_job(&job_info_general, job_id, 0);

    if (load_result == SLURM_SUCCESS && job_info_general->record_count == 1)
    {
        nodes = job_info_general->job_array[0].num_nodes;
    }

    if (job_info_general)
    {
        slurm_free_job_info_msg(job_info_general);
    }

    return nodes;
}

/**
 * @brief Merge a pending expander job into the main job
 *
 * Requires elevated Slurm privileges and a legacy Slurm version
 *
 * @returns true if all Slurm API calls completed successfully, otherwise false
 */
bool merge_expander_job(DMRControllerState *controller_state)
{
    /*
     * Legacy Slurm approach: real resizing of jobs
     * Set merger jobs to have 0 nodes, then kill it
     * Finally, let the main job have all the resources
     */

    debug_output("Merging expander job %" PRIu32 " into %" PRIu32 "\n", controller_state->expanding_jobid, controller_state->main_jobid);

    bool success = force_set_job_nodes(controller_state->expanding_jobid, 0);

    if (success)
    {
        success = kill_job(controller_state->expanding_jobid);
    }

    if (success)
    {
        success = force_set_job_nodes(controller_state->main_jobid, INFINITE);
    }

    if (success)
    {
        // We reach this point before we have added the new nodes into our count, so calculate it here
        int expected_new_nodes = dmr_get_current_node_count() + dmr_get_nodes_next_expand();

        debug_output("Force set nodes of job %" PRIu32 " to %d\n", controller_state->main_jobid, expected_new_nodes);

        success = force_set_job_nodes(controller_state->main_jobid, expected_new_nodes);
    }

    return success;
}

/**
 * @brief Indicate whether or not we should try to use Slurm job-growing functionality
 *
 * @returns true if jobs_can_shrink() returns true and JOBS_CAN_GROW is 1, otherwise false
 */
bool jobs_can_grow(void)
{
    return jobs_can_shrink() && JOBS_CAN_GROW == 1;
}

/**
 * @brief Indicate whether or not we should try to use Slurm job-shrinking functionality
 *
 * @returns true if JOBS_CAN_SHRINK is 1, otherwise false
 */
bool jobs_can_shrink(void)
{
    return JOBS_CAN_SHRINK == 1;
}

/**
 * @brief Take all hostlists in the controller state and move them to the returned hostlist
 *
 * @warning This will destroy the existing hostlists in the controller state
 */
compat_hostlist_t merge_all_hostlists(DMRControllerState *controller_state)
{
    compat_hostlist_t new_hostlist = slurm_hostlist_create(NULL);

    for (int i = 0; i < controller_state->jobs_count_alive; i++)
    {
        move_hosts(new_hostlist, controller_state->jobs_info[i].hostlist);
        hostlist_destroy_set_null(&controller_state->jobs_info[i].hostlist);
    }

    if(controller_state->expanding_hostlist && slurm_hostlist_count(controller_state->expanding_hostlist) != 0)
    {
        move_hosts(new_hostlist, controller_state->expanding_hostlist);
        hostlist_destroy_set_null(&controller_state->expanding_hostlist);
    }

    return new_hostlist;
}

/**
 * @brief Kill any hanging expander jobs or shrink them if they are holding more nodes than needed
 */
void slurm_dmr_reconfigure(DMRControllerState *controller_state)
{
    int total_jobs = controller_state->jobs_count_alive + controller_state->jobs_count_dead;

    // Start at the first alive job, it may need to shrink
    for(int i = controller_state->jobs_count_alive-1; i<total_jobs; i++)
    {
        if(controller_state->jobs_info[i].should_kill)
        {
            debug_output("Killing job ID %" PRIu32 "\n", controller_state->jobs_info[i].job_id);
            bool killed = kill_job(controller_state->jobs_info[i].job_id);

            if(!killed)
            {
                dmr_error("Failed to kill job ID %" PRIu32 "\n", controller_state->jobs_info[i].job_id);
            }
        }
        else if(controller_state->jobs_info[i].should_shrink)
        {
            debug_output("Shrinking job ID %" PRIu32 " to %d node(s)\n", controller_state->jobs_info[i].job_id, controller_state->jobs_info[i].host_count);
            
            bool shrunk = force_set_job_nodelist(controller_state->jobs_info[i].job_id, controller_state->jobs_info[i].host_count, controller_state->jobs_info[i].hostlist);
           
            if(!shrunk)
            {
                dmr_error("Failed to shrink job ID %" PRIu32 "\n", controller_state->jobs_info[i].job_id);
            }
        }
    }
}

/**
 * @brief Readjust a scheduled expander job's end time to reflect the time it has lost pending in the queue
 *
 * @returns true if no fatal errors occurred, otherwise false
 */
bool adjust_pending_job_time(DMRControllerState *controller_state)
{
    if (difftime(time(NULL), controller_state->expansion_check_time) > MINUTE_IN_SECONDS)
    {
        controller_state->expansion_check_time = time(NULL);
        debug_output("About to adjust end time of job.\n");
        bool success = adjust_job_end_time(controller_state->global_end_time, controller_state->expanding_jobid);
        debug_output("Adjusted end time of job.\n");

        if (!success)
        {
            if (get_minutes_left(controller_state->global_end_time) > 0)
            {
                int error = slurm_get_errno();
                dmr_error("Tried to adjust job end time of expansion job, but failed (%s). Ignoring...\n", slurm_strerror(error));
                return true; // Non-fatal error
            }
            else
            {
                dmr_error("Expander job, and therefore your main job, has run out of time.\n");
                controller_state->expanding_jobid = DEFAULT_SLURM_JOBID;
                return false; // Fatal error
            }
        }
    }

    return true;
}

/**
 * @brief Submit an expander batch job and update the states accordingly
 */
void expand_slurm_world(DMRState *dmr_state, DMRControllerState *controller_state)
{
    if (dmr_state->is_root_process)
    {
        if (jobs_can_grow())
        {
            // Submit a job which we intend to merge into the main job
            controller_state->expanding_jobid = alloc_expand_merger_job(controller_state->main_jobid, dmr_get_nodes_next_expand());
        }
        else
        {
            // Submit a job which we intend to just use the resources from
            job_desc_msg_t job_desc;
            fill_job_desc(&job_desc, controller_state->global_end_time, controller_state->main_jobid, dmr_get_active_expansions());
            debug_output("Submitting a batch job\n");
            controller_state->expanding_jobid = submit_batch_job(&job_desc);
            debug_output("Submitted a batch job\n");
            cleanup_job_desc(&job_desc);
        }

        controller_state->expansion_check_time = time(NULL);

        if (controller_state->expanding_jobid == DEFAULT_SLURM_JOBID)
        {
            int error = slurm_get_errno();
            dmr_error("Expansion job submission unsuccessful: \"%s\" (code %d)\n", slurm_strerror(error), error);
        }
    }
}

/**
 * @brief Destroy the given hostlist and set it to NULL
 */
void hostlist_destroy_set_null(compat_hostlist_t *the_hostlist)
{
    if(!the_hostlist)
    {
        return;
    }

    slurm_hostlist_destroy(*the_hostlist);
    *the_hostlist = NULL;
}