/**
 * @file dmr_utils.c
 * @brief DMR helper functions
 *
 * Contains helper logic used to implement the functionality of DMR,
 * not including direct interactions with Slurm (see dmr_slurm.c).
 * Not intended for external use.
 */

#include "dmr.h"
#include "dmr_internal.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define _GNU_SOURCE

// We need these values a lot to know what to print, so it is useful not to have to look them up every time
static int32_t debug_level_cache = OUTPUT_LEVEL_UNKNOWN;
static int32_t analytics_print_cache = OUTPUT_LEVEL_UNKNOWN;

/**
 * @brief Try to read PROCS_PER_NODE from environment, or return compiled default
 */
static int get_default_ppn(void)
{
    uint32_t procs_per_node;
    bool found_from_env = get_uint32_from_env(NAMEOF(DMR_PROCS_PER_NODE), &procs_per_node);

    if (found_from_env)
    {
        return procs_per_node;
    }

    return DMR_PROCS_PER_NODE;
}

/**
 * @brief Try to read NODES_IN_EXPAND from environment, or return compiled default
 */
static int get_default_nodes_in_expand(void)
{
    uint32_t nodes_in_expand;
    bool found_from_env = get_uint32_from_env(NAMEOF(DMR_NODES_IN_EXPAND), &nodes_in_expand);

    if (found_from_env)
    {
        return nodes_in_expand;
    }

    return DMR_NODES_IN_EXPAND;
}

/**
 * @brief Get the number of whole jobs that would be removed if removing nodes_to_remove nodes
 */
static int calc_jobs_to_remove(DMRControllerState *controller_state, int nodes_to_remove)
{
    if(nodes_to_remove <= 0)
    {
        return 0;
    }

    int nodes_removed = 0;
    int jobs = 0;

    for(int i = controller_state->jobs_count_alive-1; i>0; i--)
    {
        int nodes_in_job = controller_state->jobs_info[i].host_count;

        if(nodes_removed+nodes_in_job>nodes_to_remove)
        {
            break;
        }
        
        nodes_removed+=nodes_in_job;
        jobs++;
    }

    return jobs;
}

/**
 * @brief Get the number of nodes that would be removed if terminating jobs_to_remove jobs
 */
static int calc_nodes_to_remove(DMRControllerState *controller_state, int jobs_to_remove)
{
    // Cannot terminate the non-expander job fully
    if(jobs_to_remove > dmr_get_active_expansions())
    {
        return 0;
    }

    int nodes = 0;

    for(int i = controller_state->jobs_count_alive-1; i>0; i--)
    {
        nodes += controller_state->jobs_info[i].host_count;
    }

    debug_output("%s: removing %d jobs will remove %d nodes\n", __func__, jobs_to_remove, nodes);

    return nodes;
}

/**
 * @brief Load a uint32_t into the_number from a given char array
 *
 * @returns true when completed successfully, false otherwise
 */
static bool parse_uint32_from_string(char *string, uint32_t *the_number)
{
    if (!string || !the_number)
    {
        return false;
    }

    char *endptr;
    errno = 0;

    unsigned long ulong_val = strtoul(string, &endptr, 10);

    // Reject if any parsing error occurred
    if (*endptr != '\0' || errno == ERANGE || ulong_val > UINT32_MAX)
    {
        return false;
    }

    // The entire string was parsed to a number correctly
    (*the_number) = (uint32_t)ulong_val;

    return true;
}

/**
 * @brief Load a uint32_t into the_number from a given environment_var
 *
 * @returns true when completed successfully, false otherwise
 */
bool get_uint32_from_env(char *environment_var, uint32_t *the_number)
{
    char *number_as_string = getenv(environment_var);

    return parse_uint32_from_string(number_as_string, the_number);
}

/**
 * @brief Load a int32_t into the_number from a given char array
 *
 * @returns true when completed successfully, false otherwise
 */
static bool parse_int32_from_string(char *string, int32_t *the_number)
{
    if (!string || !the_number)
    {
        return false;
    }

    char *endptr;
    errno = 0;

    long long_val = strtol(string, &endptr, 10);

    // Reject if any parsing error occurred
    if (*endptr != '\0' || errno == ERANGE || long_val < INT32_MIN || long_val > INT32_MAX)
    {
        return false;
    }

    *the_number = (int32_t)long_val;
    return true;
}

/**
 * @brief Load a int32_t into the_number from a given environment_var
 *
 * @returns true when completed successfully, false otherwise
 */
static bool get_int32_from_env(char *environment_var, int32_t *the_number)
{
    char *number_as_string = getenv(environment_var);
    return parse_int32_from_string(number_as_string, the_number);
}

/**
 * @brief Load a double precision value into the_number from a given string
 *
 * @returns true when completed successfully, false otherwise
 */
static bool parse_double_from_string(char *string, double *the_number)
{
    if (!string || !the_number)
    {
        return false;
    }

    char *endptr;
    errno = 0;

    double val = strtod(string, &endptr);

    if (*endptr != '\0' || errno == ERANGE || !isfinite(val))
    {
        return false;
    }

    *the_number = val;
    return true;
}

/**
 * @brief Load a double precision value into the_number from a given environment_var
 *
 * @returns true when completed successfully, false otherwise
 */
bool get_double_from_env(char *environment_var, double *the_number)
{
    char *number_as_string = getenv(environment_var);

    return parse_double_from_string(number_as_string, the_number);
}

/**
 * @brief Try to read DMR_DEBUG_LEVEL from environment, or return compiled default
 */
static int get_debug_level(void)
{
    if(debug_level_cache != OUTPUT_LEVEL_UNKNOWN)
    {
        return debug_level_cache;
    }

    int32_t debug_level;
    bool found_from_env = get_int32_from_env(NAMEOF(DMR_DEBUG_LEVEL), &debug_level);

    if (found_from_env)
    {
        debug_level_cache = debug_level;
        return debug_level_cache;
    }

    debug_level_cache = DMR_DEBUG_LEVEL;
    return DMR_DEBUG_LEVEL;
}

/**
 * @brief Print a given output to stdout
 */
void dmr_output(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

/**
 * @brief Print analytics information if analytics are enabled
 */
void dmr_analytics(DMRState *dmr_state, DMRControllerState *controller_state, double current_time, double communication_efficiency, char const *function, char const *state, bool print_reconfig_time)
{
    if (!dmr_state->is_root_process || !should_print_analytics())
    {
        return;
    }

    double reconf_time = -1.0;

    if (print_reconfig_time && current_time > 0 && controller_state->reconfig_start_last > 0)
    {
        reconf_time = current_time - controller_state->reconfig_start_last;
    }

    // Format:
    //[DMR ANALYTICS],<current time>,<function>,<state>,<world size>,<node count>,<reconfiguration time>,<CE>

    printf("[DMR ANALYTICS],%f,%s,%s,%d,%d,%.2f,%f\n",
           current_time,
           function,
           state,
           dmr_state->local_comm_size,
           dmr_state->current_nodes,
           reconf_time,
           communication_efficiency);
}

/**
 * @brief Print a debug statement if an appropriate debug level is activated
 */
void debug_output(const char *format, ...)
{
    int debug_level = get_debug_level();

    if (debug_level <= NEVER_PRINT_OUTPUT)
    {
        return;
    }

    int mpi_initialized;
    MPI_Initialized(&mpi_initialized);

    int mpi_finalized;
    MPI_Finalized(&mpi_finalized);

    int my_rank = -1;
    int name_length;
    char processor_name[MPI_MAX_PROCESSOR_NAME] = "UNKNOWN";

    if (mpi_initialized && !mpi_finalized)
    {
        MPI_Get_processor_name(processor_name, &name_length);
        MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    }

    if (debug_level == PRINT_FROM_PROC_ZERO && my_rank != PROC_ZERO)
    {
        return;
    }

    int reconfig_count = dmr_get_reconfig_count();

    char *prefix = "[RANK%d:%s RECONFIG#%d] ";

    va_list args;
    va_list args_cpy;
    va_start(args, format);
    va_copy(args_cpy, args); // Copy, so we can call vsnprintf twice

    // Get the sizes we need to allocate memory for
    int prefix_length = snprintf(NULL, 0, prefix, my_rank, processor_name, reconfig_count);
    int msg_length = vsnprintf(NULL, 0, format, args);
    int total_length = prefix_length + msg_length + NULLCHAR_LEN;

    va_end(args);

    // Write the full message into print_output
    char *print_output = malloc(sizeof(char) * total_length);

    if (!print_output)
    {
        dmr_error("Memory allocation issue when printing debug output\n");
        return;
    }

    snprintf(print_output, total_length, prefix, my_rank, processor_name, reconfig_count);

    vsnprintf(print_output + prefix_length, total_length - prefix_length, format, args_cpy);

    va_end(args_cpy);

    // Ready to print
    printf("%s", print_output);

    free(print_output);
}

/**
 * @brief Print given output to stderr
 */
void dmr_error(const char *format, ...)
{
    fprintf(stderr, DMR_ERROR_PREFIX);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

/**
 * @brief Call MPI_Abort(MPI_COMM_WORLD) and exit
 */
void dmr_abort(void)
{
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    exit(EXIT_FAILURE);
}

/**
 * @brief Get a string representation of the given DMRSuggestion enum
 *
 * @returns The DMRSuggestion enum value represented as a string
 */
char const *suggestion_to_string(DMRSuggestion suggestion)
{
    switch (suggestion)
    {
    case ROUND_POLICY:
        return "ROUND POLICY";
    case CE_POLICY:
        return "CE POLICY";
    case SLURM4DMR_ROUND_POLICY:
        return "SLURM4DMR ROUND POLICY";
    case SLURM4DMR_CE_POLICY:
        return "SLURM4DMR CE POLICY";
    case SLURM4DMR_QUEUE_POLICY:
        return "SLURM4DMR QUEUE POLICY";
    case SHOULD_EXPAND:
        return "SHOULD EXPAND";
    case SHOULD_SHRINK:
        return "SHOULD SHRINK";
    case SHOULD_STAY:
        return "SHOULD STAY";
    case ROUTE_UNDEF_STEWARDSHIP:
        return "ROUTE UNDEF STEWARDSHIP";
    case QCUT_PIPELINE_BALANCE:
        return "QCUT PIPELINE BALANCE";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Get a string representation of the given InternalAction enum
 *
 * @returns The InternalAction enum value represented as a string
 */
char const *internal_action_to_string(InternalAction action)
{
    switch (action)
    {
    case NONE:
        return "NONE";
    case EXPAND_WAIT_FOR_SLURM:
        return "EXPAND_WAIT_FOR_SLURM";
    case EXPAND_READY_RECONFIG:
        return "EXPAND_READY_RECONFIG";
    case RESTARTED_NEED_DATA:
        return "RESTARTED_NEED_DATA";
    case SHRINKING:
        return "SHRINKING";
    case SHRINKING_FINALIZING:
        return "SHRINKING_FINALIZING";
    case EXPANDED_FINALIZING:
        return "EXPANDED_FINALIZING";
    default:
        return "UNKNOWN_ACTION";
    }
}

/**
 * @brief Get the minutes left from current time until a given time_t end time
 *
 * @returns Time, in minutes, until end_time
 */
int get_minutes_left(time_t end_time)
{
    double time_left_s = difftime(end_time, time(NULL));
    double time_left_min = time_left_s / 60.0;

    // Use ceil to give a conservative estimate; we do not want to prematurely terminate an expander job
    return ceil(time_left_min);
}

/**
 * @brief Initialize a DMRState struct with default values
 */
void init_dmr_state(DMRState **dmr_state)
{
    *dmr_state = malloc_or_abort(sizeof(DMRState));
    (*dmr_state)->dmr_is_initialized = false;
    (*dmr_state)->dmr_soft_crash = false;
    (*dmr_state)->is_root_process = false;
    (*dmr_state)->local_comm_rank = -1;
    (*dmr_state)->local_comm_size = -1;
    (*dmr_state)->expansion_request = NULL;
    (*dmr_state)->action_in_progress = NONE;
    (*dmr_state)->INTERNAL_COMM_WORLD = MPI_COMM_NULL;
    (*dmr_state)->current_nodes = 0;
    (*dmr_state)->reconf_step_inhibitor = 0;
    (*dmr_state)->current_reconf_step_inhibitor = 0;
    (*dmr_state)->policy_min_nodes = 1;
    (*dmr_state)->policy_max_nodes = 1;
    (*dmr_state)->policy_stride = 2;
    (*dmr_state)->policy_pref_nodes = 1;
}

/**
 * @brief Clean up malloced resources, communicators, and free DMRState
 */
void free_dmr_state(DMRState *dmr_state)
{
    MPI_Comm_free(&dmr_state->INTERNAL_COMM_WORLD);

    free(dmr_state);
}

/**
 * @brief Initialize a DMRControllerState struct with default values
 */
void init_controller_state(DMRControllerState **controller_state)
{
    *controller_state = malloc_or_abort(sizeof(DMRControllerState));
    (*controller_state)->global_end_time = (time_t)(-1);
    (*controller_state)->expansion_check_time = (time_t)(-1);
    (*controller_state)->arg_count = 0;
    (*controller_state)->arg_array = NULL;
    (*controller_state)->executable_name = NULL;
    (*controller_state)->slurm_userid = UINT32_MAX;
    (*controller_state)->main_jobid = DEFAULT_SLURM_JOBID;
    (*controller_state)->expanding_jobid = DEFAULT_SLURM_JOBID;
    (*controller_state)->expand_start_run = (time_t)(-1);
    (*controller_state)->expanding_hostlist = NULL;
    (*controller_state)->nodes_in_next_expand = get_default_nodes_in_expand();
    (*controller_state)->procs_in_next_expand = -1;
    (*controller_state)->procs_per_node = get_default_ppn();
    (*controller_state)->jobs_count_alive = 0;
    (*controller_state)->jobs_count_dead = 0;
    (*controller_state)->new_node_count = 0;
    (*controller_state)->reconfig_start_curr = -1.0;
    (*controller_state)->reconfig_start_last = -1.0;
    (*controller_state)->nodes_next_shrink = 0;
    (*controller_state)->procs_next_shrink = 0;
    (*controller_state)->dqr_ctx = NULL;
    (*controller_state)->jobs_info = malloc_or_abort(sizeof(DMRSlurmJobInfo) * MAX_EXPANSIONS);

    for (int i = 0; i < MAX_EXPANSIONS; i++)
    {
        (*controller_state)->jobs_info[i].hostlist = NULL;
        (*controller_state)->jobs_info[i].proc_counts = NULL;
        (*controller_state)->jobs_info[i].should_kill = false;
        (*controller_state)->jobs_info[i].should_shrink = false;
    }
}

/**
 * @brief Clean up malloced resources and free DMRControllerState
 */
void free_controller_state(DMRControllerState *dmr_controller)
{
    if (!dmr_controller) return;

    for (int i = 0; i < MAX_EXPANSIONS; i++)
    {
        if (dmr_controller->jobs_info[i].hostlist)
        {
            slurm_hostlist_destroy(dmr_controller->jobs_info[i].hostlist);
        }

        if (dmr_controller->jobs_info[i].proc_counts)
        {
            free(dmr_controller->jobs_info[i].proc_counts);
        }
    }

    if (dmr_controller->dqr_ctx)
    {
        dqr_destroy(dmr_controller->dqr_ctx);
        dmr_controller->dqr_ctx = NULL;
    }

    free(dmr_controller->jobs_info);

    if (dmr_controller->expanding_hostlist)
    {
        slurm_hostlist_destroy(dmr_controller->expanding_hostlist);
    }

    if (dmr_controller->arg_array)
    {
        for (int i = 0; i < dmr_controller->arg_count; i++)
        {
            free(dmr_controller->arg_array[i]);
        }
        free(dmr_controller->arg_array);
    }

    if (dmr_controller->executable_name)
    {
        free(dmr_controller->executable_name);
    }

    free(dmr_controller);
}

/**
 * @brief Free the DMR intercommunicator if it exists, printing debug giving the specified side
 */
void free_dmr_intercomm(char *side)
{
    if (DMR_INTERCOMM != MPI_COMM_NULL)
    {
        debug_output("About to free DMR_INTERCOMM on %s side\n", side);

        MPI_Comm_free(&DMR_INTERCOMM);

        debug_output("Freed DMR_INTERCOMM on %s side\n", side);
    }
}

/**
 * @brief Use a given DMRState pointer dmr_state to determine whether DMR was initialized.
 *
 * The pointer to bool print_error is filled with a value indicating
 * if the calling process should print output (true) or not (false)
 *
 * @returns Returns false if not initialized or if dmr_state is null
 */
bool check_dmr_initialized(DMRState *dmr_state, bool *print_error)
{
    *print_error = false;

    if (!dmr_state)
    {
        int mpi_initialized;
        MPI_Initialized(&mpi_initialized);
        int local_comm_rank = 0;

        if (mpi_initialized)
        {
            MPI_Comm_rank(MPI_COMM_WORLD, &local_comm_rank);
        }

        // Try to print the error on proc zero (prints for all if not initialized)
        if (local_comm_rank == PROC_ZERO)
        {
            *print_error = true;
        }

        return false;
    }

    return true;
}

/**
 * @brief Determine the new values for active expansions and new reconfigurations based on the current state
 */
void get_counts_post_reconfig(DMRState *dmr_state, DMRControllerState *controller_state, int *new_expansions, int *new_reconfigs)
{
    bool expanding = dmr_state->action_in_progress == EXPAND_READY_RECONFIG;

    int old_expansion_count = dmr_get_active_expansions();

    if (old_expansion_count < 0)
    {
        dmr_error("Read invalid expansion count from environment...\n");
        old_expansion_count = 0;
    }

    int old_reconfig_count = dmr_get_reconfig_count();

    if (old_reconfig_count < 0)
    {
        dmr_error("Read invalid reconfiguration count from environment...\n");
        old_reconfig_count = 0;
    }

    int new_reconfig_count = old_reconfig_count + 1;
    int new_expansion_count;

    // If we can directly edit job sizes in both directions (implied if we can grow), then we do not ever have expander jobs active when spawning
    if (jobs_can_grow())
    {
        new_expansion_count = 0;
    }

    // We always expand by exactly one job regardless of the DMR configuration
    else if(expanding)
    {
        new_expansion_count = old_expansion_count + 1;
    }

    else
    {
        int jobs_removed = calc_jobs_to_remove(controller_state, dmr_get_nodes_next_shrink());
        new_expansion_count = old_expansion_count - jobs_removed;
        debug_output("Shrinking by %d nodes which means %d jobs, so new expansion count is %d\n", dmr_get_nodes_next_shrink(), jobs_removed, new_expansion_count);
    }

    if (new_expansions)
    {
        *new_expansions = new_expansion_count;
    }

    if (new_reconfigs)
    {
        *new_reconfigs = new_reconfig_count;
    }
}

/**
 * @brief Indicate whether or not we have compiled in a way where we are reconfigure with the checkpoint-restart mechanism
 *
 * @returns true if CHECKPOINT_RESTART is 1, otherwise false
 */
bool use_checkpoint_restart(void)
{
    return CHECKPOINT_RESTART == 1;
}

/**
 * @brief Get the number of processes that would be removed if we shrunk by the specified node count
 *
 * @returns The number of processes due to be removed if we shrank by specified node count
 */
int get_procs_removed_in_shrink(DMRControllerState *controller_state, int nodes)
{
    if (nodes == 0)
    {
        return 0;
    }

    int procs_removing = 0;
    int nodes_removing = 0;

    for (int i = controller_state->jobs_count_alive - 1; i >= 0; i--)
    {
        int nodes_in_job = controller_state->jobs_info[i].host_count;

        for (int j = nodes_in_job - 1; j >= 0; j--)
        {
            nodes_removing++;
            procs_removing += controller_state->jobs_info[i].proc_counts[j];

            if (nodes_removing == nodes)
            {
                return procs_removing;
            }
        }
    }

    return procs_removing;
}

/**
 * @brief Get the number of nodes that would be removed if we shrunk by the specified process count
 *
 * @returns The number of nodes due to be removed if we shrank by specified process count
 */
int get_nodes_removed_in_shrink(DMRControllerState *controller_state, int procs)
{
    int procs_removing = 0;
    int nodes_removing = 0;

    for (int i = controller_state->jobs_count_alive - 1; i >= 0; i--)
    {
        int nodes_in_job = controller_state->jobs_info[i].host_count;

        for (int j = 0; j < nodes_in_job; j++)
        {
            procs_removing += controller_state->jobs_info[i].proc_counts[j];

            // In cases where procs_removing == procs, we want to remove that node, so continue
            if (procs_removing > procs)
            {
                return nodes_removing;
            }

            nodes_removing++;
        }
    }

    return nodes_removing;
}

/**
 * @brief Given a 0-based node index and a total node / process count, get how many processes that node is assigned
 *
 * @returns Number of processes assigned to node at index node_idx
 */
int get_proc_count_for_node(int total_procs, int total_nodes, int node_idx)
{
    /*
     * This helper function avoids inconsistencies when making the
     * calculation of how many processes to assign per node. The simple rule
     * is to divide evenly first and give a single remainder process per node,
     * from low index to high index, until there are no more remainder processes left
     */

    int base_count = total_procs / total_nodes;
    int remainder = total_procs % total_nodes;

    if (node_idx < remainder)
    {
        return base_count + 1;
    }

    if (base_count <= 0)
    {
        dmr_error("Encountered an issue getting process counts. Unexpected behavior may follow.\n");
        base_count = 1;
    }

    return base_count;
}

/**
 * @brief Match the nodes in the array procs_per_node with corresponding jobs and set the process count
 */
void set_existing_proc_counts(DMRControllerState *controller_state, int max_nodes, int *procs_per_node, int procs_in_reconfig, int *added_procs, int *added_nodes)
{
    *added_procs = 0;
    *added_nodes = 0;

    /*
     * Recreate the same process-per-node structure as currently exists.
     * Fill in process counts here which we can respawn exactly as they were to make procs_in_reconfig,
     * leaving any others untouched
     */
    for (int i = 0; i < controller_state->jobs_count_alive; i++)
    {
        int nodes_in_job = controller_state->jobs_info[i].host_count;
        int procs_in_job = controller_state->jobs_info[i].total_procs;

        // This might have changed, but we are interested in the original configuration
        debug_output("Job ID %" PRIu32 " has (or had) %d nodes and %d processes.\n", controller_state->jobs_info[i].job_id, nodes_in_job, procs_in_job);

        for (int j = 0; j < nodes_in_job; j++)
        {
            int procs_for_node = controller_state->jobs_info[i].proc_counts[j];

            if (*added_procs + procs_for_node <= procs_in_reconfig)
            {
                *added_procs += procs_for_node;
                procs_per_node[*added_nodes] = procs_for_node;
                (*added_nodes)++;
            }
            else
            {
                return;
            }

            if (*added_nodes >= max_nodes)
            {
                return;
            }
        }
    }
}

/**
 * @brief Create an MPI_Datatype to broadcast information from root when initializing.
 *
 * Free the returned datatype with MPI_Type_free when no longer needed
 */
void create_info_datatype(MPI_Datatype *info_datatype)
{
    /*
     * A datatype to send some data from root to the rest of the processes.
     * This is a bit more complex than it needs to be, but it can be extended to
     * arbitrary datatypes easily if needed
     */

    MPI_Datatype types[] = {MPI_INT};
    int block_lengths[] = {INTEGERS_FROM_ROOT_COUNT};

    MPI_Aint displacements[TYPES_FROM_ROOT_COUNT];
    displacements[0] = offsetof(DMRInitialInfo, int_data);

    MPI_Type_create_struct(TYPES_FROM_ROOT_COUNT, block_lengths, displacements, types, info_datatype);

    MPI_Type_commit(info_datatype);
}

/**
 * @brief Try to read NODES_IN_SHRINK from environment, or return compiled default
 */
int get_default_nodes_in_shrink(void)
{
    uint32_t nodes_in_shrink;
    bool found_from_env = get_uint32_from_env(NAMEOF(DMR_NODES_IN_SHRINK), &nodes_in_shrink);

    if (found_from_env)
    {
        return nodes_in_shrink;
    }

    return DMR_NODES_IN_SHRINK;
}

/**
 * @brief Try to read DMR_BLOCKING_REQ from environment, or return compiled default
 */
bool requests_are_blocking(void)
{
    uint32_t blocking_reconfigs;
    bool found_from_env = get_uint32_from_env(NAMEOF(DMR_BLOCKING_REQ), &blocking_reconfigs);

    if (found_from_env)
    {
        return blocking_reconfigs != 0;
    }

    return DMR_BLOCKING_REQ;
}

/**
 * @brief Verify that we could reasonably remove resources in the suggested way
 *
 * @returns true if we can remove resources in current configuration, otherwise false
 */
bool can_remove_resources(DMRState *dmr_state)
{
    // Information is held by root process, but all processes need it
    int procs_in_shrink;

    if (dmr_state->is_root_process)
    {
        procs_in_shrink = dmr_get_procs_next_shrink();
    }

    MPI_Bcast(&procs_in_shrink, 1, MPI_INT, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD);

    // No resources to remove

    if (procs_in_shrink == 0)
    {
        return false;
    }

    // Feasible as long as it leaves us with some processes alive
    return dmr_state->local_comm_size > procs_in_shrink;
}

/**
 * @brief Check whether we should print analytics
 *
 * Read DMR_PRINT_ANALYTICS from environment (prioritized) or return compiled
 * value if none is found.
 *
 * @returns The value of DMR_PRINT_ANALYTICS
 */
bool should_print_analytics(void)
{
    if(analytics_print_cache != OUTPUT_LEVEL_UNKNOWN)
    {
        return analytics_print_cache != NEVER_PRINT_OUTPUT;
    }

    int32_t print_analytics;
    bool found_from_env = get_int32_from_env(NAMEOF(DMR_PRINT_ANALYTICS), &print_analytics);

    if (!found_from_env)
    {
        print_analytics = DMR_PRINT_ANALYTICS;
    }

    if (print_analytics == 0)
    {
        analytics_print_cache = NEVER_PRINT_OUTPUT;
        return NEVER_PRINT_OUTPUT;
    }

    else
    {
        analytics_print_cache = PRINT_FROM_PROC_ZERO;
        return PRINT_FROM_PROC_ZERO;
    }
}


/**
 * @brief Get the system's time as a double, for use across reconfigurations
 *
 * @returns The time of the system as a double
 */
double get_global_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/**
 * @brief Create an MPI datatype for the NodeInfo struct
 *
 * @warning Caller is responsible for freeing the MPI_Datatype created
 */
void create_nodeinfo_datatype(MPI_Datatype *the_datatype)
{
    MPI_Datatype types[] = {MPI_CHAR, MPI_INT};
    int block_lengths[] = {MPI_MAX_PROCESSOR_NAME, 1};

    int const info_count = 2;

    MPI_Aint displacements[info_count];
    displacements[0] = offsetof(NodeInfo, node_name);
    displacements[1] = offsetof(NodeInfo, proc_count);

    MPI_Type_create_struct(info_count, block_lengths, displacements, types, the_datatype);
    MPI_Type_commit(the_datatype);
}

/**
 * @brief Take a hostname and return its IP
 * @returns Dynamically allocated memory with IP which must be freed by caller.
 * @note If lookup fails, hostname is copied and returned.
 */
char *hostname_to_ip(const char *hostname)
{
    struct addrinfo hints = {0}, *res;
    char ipstr[INET6_ADDRSTRLEN];
    void *addr_ptr = NULL;
    char *normalized = NULL;
    bool found = false;

    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // Looking for a TCP address

    if (getaddrinfo(hostname, NULL, &hints, &res) == 0)
    {
        if (res->ai_family == AF_INET)
        {
            addr_ptr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        }
        else if (res->ai_family == AF_INET6)
        {
            addr_ptr = &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr;
        }

        if (addr_ptr && inet_ntop(res->ai_family, addr_ptr, ipstr, sizeof(ipstr)))
        {
            found = true;
            asprintf_or_abort(&normalized, "%s", ipstr);
        }

        freeaddrinfo(res);
    }

    if (!found)
    {
        debug_output("Failed to resolve IP for host %s, so returning it as-is\n", hostname);
        asprintf_or_abort(&normalized, "%s", hostname);
    }

    return normalized;
}

/**
 * @brief Allocate the requested memory or abort DMR if failed
 */
void *malloc_or_abort(size_t size)
{
    if (size == 0)
    {
        dmr_error("Passed size_t = 0 to %s. Aborting.\n", __func__);
        dmr_abort();
    }

    void *allocated = malloc(size);

    if (!allocated)
    {
        dmr_error("Memory allocation issue detected by %s. Aborting.\n", __func__);
        dmr_abort();
    }

    return allocated;
}

/**
 * @brief Duplicate the requested string or abort DMR if failed
 */
char *strdup_or_abort(const char *string)
{
    if (!string)
    {
        dmr_error("Passed NULL pointer to %s. Aborting.\n", __func__);
        dmr_abort();
    }

    char *copy = strdup(string);

    if (!copy)
    {
        dmr_error("Memory allocation issue detected by %s. Aborting.\n", __func__);
        dmr_abort();
    }
    return copy;
}

/**
 * @brief Allocate memory for and print the requested string or abort DMR if failed
 */
void asprintf_or_abort(char **output, const char *format, ...)
{
    if (!output)
    {
        dmr_error("Passed NULL pointer to %s. Aborting.\n", __func__);
        dmr_abort();
    }

    va_list args;
    va_start(args, format);
    int ret = vasprintf(output, format, args);
    va_end(args);

    if (ret == -1)
    {
        dmr_error("Memory allocation issue detected by %s. Aborting.\n", __func__);
        dmr_abort();
    }
}

/**
 * @brief Allocate memory for and print the requested string or make string NULL if failed
 */
void asprintf_or_null(char **output, const char *format, ...)
{
    if (!output)
    {
        dmr_error("Passed NULL pointer to %s. Aborting.\n", __func__);
        dmr_abort();
    }

    va_list args;
    va_start(args, format);
    int ret = vasprintf(output, format, args);
    va_end(args);

    if (ret == -1)
    {
        dmr_error("Memory allocation issue detected by %s.\n", __func__);
        *output = NULL;
    }
}

/**
 * @brief Get the number of jobs to terminate to remove as close to nodes_to_remove as possible.
 * 
 * Intended for configurations where we cannot shrink jobs 
 */
int get_nearest_jobs_to_nodes(DMRControllerState *controller_state, int nodes_to_remove)
{
    if(dmr_get_active_expansions() <= 0 || nodes_to_remove <= 0)
    {
        return 0;
    }
    
    int distance_to_target_prev = nodes_to_remove; 

    int nodes_removed = 0;

    int jobs = 0;

    for(int i = dmr_get_active_expansions(); i>0; i--)
    {
        nodes_removed += controller_state->jobs_info[i].host_count;

        int distance_to_target = abs(nodes_to_remove - nodes_removed);
        
        if(distance_to_target_prev <= distance_to_target)
        {
            break;
        }

        distance_to_target_prev = distance_to_target;
        jobs++;
    }

    return jobs;
}

/**
 * @brief Set number of jobs to shrink by without printing error messages
 * 
 * Useful for policies which need to set this value where it can sometimes result in 
 * a policy which shrinks by 0 jobs. This is converted to SHOULD_STAY later in dmr_check.
 * Also used after checks are completed in the main @ref dmr_set_jobs_next_shrink function.
 */
void internal_set_jobs_next_shrink(DMRControllerState *controller_state, int jobs)
{
    int nodes_removed = calc_nodes_to_remove(controller_state, jobs);

    controller_state->procs_next_shrink = get_procs_removed_in_shrink(controller_state, nodes_removed);
    controller_state->nodes_next_shrink = nodes_removed;
}

/**
 * @brief Trim leading and trailing whitespace from a string in-place.
 *
 * Removes trailing '\n', '\r' and any isspace() characters,
 * then removes leading whitespace by shifting the string.
 *
 * @param s Null-terminated string to trim (may be NULL).
 */
static void dmr_trim_inplace(char *s)
{
    if(!s) return;
    size_t n = strlen(s);
    while(n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || isspace((unsigned char)s[n-1])))
    {
        s[n-1] = '\0';
        n--;
    }
    size_t i = 0;
    while(s[i] && isspace((unsigned char)s[i])) i++;
    if(i > 0) memmove(s, s+i, strlen(s+i)+1);
}

/**
 * @brief Case-insensitive string equality check.
 *
 * Compares two null-terminated strings using ASCII case folding.
 *
 * @param a First string (may be NULL).
 * @param b Second string (may be NULL).
 * @return 1 if both strings are equal (case-insensitive), 0 otherwise.
 */
static int dmr_str_ieq(const char *a, const char *b)
{
    if(!a || !b) return 0;
    while(*a && *b)
    {
        if(tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/**
 * @brief Resolve CSV path for labels.
 *
 * Order:
 *  1) DMR_QCUT_LABELS_FILE
 *  2) <job_dir>/dmr_labels.csv
 *
 * @param job_dir Base directory for the QCUT job (may be NULL).
 * @param out_path Output buffer for resolved path.
 * @param out_sz Size of @p out_path in bytes.
 * @return 1 if a path was resolved and written to @p out_path, 0 otherwise.
 */
static int dmr_qcut_resolve_labels_csv(const char *job_dir, char *out_path, size_t out_sz)
{
    if(!out_path || out_sz == 0) return 0;
    out_path[0] = '\0';

    const char *direct = getenv("DMR_QCUT_LABELS_FILE");
    if(direct && direct[0] != '\0')
    {
        snprintf(out_path, out_sz, "%s", direct);
        return 1;
    }

    if(job_dir && job_dir[0] != '\0')
    {
        snprintf(out_path, out_sz, "%s/dmr_labels.csv", job_dir);
        return 1;
    }

    return 0;
}

/**
 * @brief Find index of "label" column in CSV header.
 * Returns -1 if not found.
 *
 * @param header_line CSV header line.
 * @return 0-based column index if found, -1 otherwise.
 */
static int dmr_qcut_find_label_col(const char *header_line)
{
    if(!header_line) return -1;

    // Make a local copy for tokenization
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", header_line);

    int idx = 0;
    char *save = NULL;
    for(char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save), idx++)
    {
        dmr_trim_inplace(tok);
        // Accept "label" exact (case-insensitive)
        if(dmr_str_ieq(tok, "label"))
        {
            return idx;
        }
    }
    return -1;
}

/**
 * @brief Extract Nth comma-separated token from a CSV line (0-based).
 *
 * Copies the token into @p out_tok and trims whitespace in-place.
 *
 * @param line Input CSV line.
 * @param n 0-based token index.
 * @param out_tok Output buffer for token.
 * @param out_sz Size of @p out_tok in bytes.
 * @return 1 if the token was found and written, 0 otherwise.
 */
static int dmr_qcut_get_csv_token_n(const char *line, int n, char *out_tok, size_t out_sz)
{
    if(!line || !out_tok || out_sz == 0 || n < 0) return 0;

    int idx = 0;
    const char *p = line;
    const char *start = p;

    while(*p)
    {
        if(*p == ',')
        {
            if(idx == n)
            {
                size_t len = (size_t)(p - start);
                if(len >= out_sz) len = out_sz - 1;
                memcpy(out_tok, start, len);
                out_tok[len] = '\0';
                dmr_trim_inplace(out_tok);
                return 1;
            }
            idx++;
            p++;
            start = p;
            continue;
        }
        p++;
    }

    // last token
    if(idx == n)
    {
        size_t len = (size_t)(p - start);
        if(len >= out_sz) len = out_sz - 1;
        memcpy(out_tok, start, len);
        out_tok[len] = '\0';
        dmr_trim_inplace(out_tok);
        return 1;
    }

    return 0;
}



/**
 * @brief Find column index in a CSV header line.
 *
 * Matches column name case-insensitively after trimming header tokens.
 *
 * @param header_line CSV header line.
 * @param col_name Column name to search.
 * @return 0-based index if found, -1 otherwise.
 */
static int dmr_qcut_find_col_idx(const char *header_line, const char *col_name)
{
    if(!header_line || !col_name) return -1;

    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", header_line);

    int idx = 0;
    char *save = NULL;
    for(char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save), idx++)
    {
        dmr_trim_inplace(tok);
        if(dmr_str_ieq(tok, col_name)) return idx;
    }
    return -1;
}

/**
 * @brief Wrapper for extracting Nth CSV token.
 *
 * @param line Input CSV line.
 * @param n 0-based token index.
 * @param out_tok Output buffer for token.
 * @param out_sz Size of @p out_tok in bytes.
 * @return 1 if found, 0 otherwise.
 */
static int dmr_qcut_csv_token_n(const char *line, int n, char *out_tok, size_t out_sz)
{
    return dmr_qcut_get_csv_token_n(line, n, out_tok, out_sz);
}

/**
 * @brief Check whether a subcircuit is already present in the row array.
 *
 * @param rows Row array.
 * @param used Number of valid rows in @p rows.
 * @param subcircuit Subcircuit id string.
 * @return 1 if duplicated, 0 otherwise.
 */
static int dmr_qcut_is_dup_subcircuit(const DMRQCutRow *rows, int used, const char *subcircuit)
{
    for(int i=0;i<used;i++)
        if(strcmp(rows[i].subcircuit, subcircuit) == 0)
            return 1;
    return 0;
}

/**
 * @brief Count label occurrences in the labels CSV.
 *
 * Reads the labels CSV (resolved via @c dmr_qcut_resolve_labels_csv) and counts
 * occurrences of labels (HPC/QC/Undefined).
 *
 * @param job_dir Base directory for the QCUT job (may be NULL if DMR_QCUT_LABELS_FILE is set).
 * @param out_counts Output struct filled with counts.
 * @return 1 on success, 0 on failure (missing file, invalid schema, I/O errors).
 */
int dmr_qcut_get_label_counts(const char *job_dir, DMRQCutLabelCounts *out_counts)
{
    if(!out_counts) return 0;

    out_counts->hpc = 0;
    out_counts->qc = 0;
    out_counts->undef = 0;
    out_counts->total = 0;

    char csv_path[PATH_MAX];
    if(!dmr_qcut_resolve_labels_csv(job_dir, csv_path, sizeof(csv_path)) || csv_path[0] == '\0')
    {
        return 0;
    }

    FILE *fp = fopen(csv_path, "r");
    if(!fp)
    {
        return 0;
    }

    char line[4096];

    // Read header and locate label column
    if(!fgets(line, sizeof(line), fp))
    {
        fclose(fp);
        return 0;
    }
    int label_col = dmr_qcut_find_label_col(line);
    if(label_col < 0)
    {
        // If schema is fixed and label is always 3rd column, you could fallback to 2
        // but safer to fail fast.
        fclose(fp);
        return 0;
    }

    // Parse rows
    while(fgets(line, sizeof(line), fp))
    {
        // Skip empty lines
        dmr_trim_inplace(line);
        if(line[0] == '\0') continue;

        char label[64];
        if(!dmr_qcut_get_csv_token_n(line, label_col, label, sizeof(label)))
        {
            continue;
        }

        // Normalize common variants
        if(dmr_str_ieq(label, "HPC"))
        {
            out_counts->hpc++;
            out_counts->total++;
        }
        else if(dmr_str_ieq(label, "QC") || dmr_str_ieq(label, "QPU"))
        {
            out_counts->qc++;
            out_counts->total++;
        }
        else if(dmr_str_ieq(label, "Undefined") || dmr_str_ieq(label, "UNDEFINED"))
        {
            out_counts->undef++;
            out_counts->total++;
        }
        else
        {
            // Unknown -> treat as Undefined conservatively
            out_counts->undef++;
            out_counts->total++;
        }
    }

    fclose(fp);
    return 1;
}

/**
 * @brief Read label rows from the labels CSV into a dynamic array.
 *
 * The CSV path is resolved via @c dmr_qcut_resolve_labels_csv. The returned array
 * must be released with @c dmr_qcut_free_labels_rows.
 *
 * Expected columns (case-insensitive): job_id, subcircuit, label, num_qubits,
 * depth, two_qubit_gates, source_json. Only subcircuit and label are required.
 *
 * @param job_dir Base directory for the QCUT job (may be NULL if DMR_QCUT_LABELS_FILE is set).
 * @param out_rows Output pointer receiving allocated array.
 * @param out_n Output count of rows in @p out_rows.
 * @return 1 on success, 0 on failure.
 */
int dmr_qcut_read_labels_rows(const char *job_dir, DMRQCutRow **out_rows, int *out_n)
{
    if(!out_rows || !out_n) return 0;
    *out_rows = NULL;
    *out_n = 0;

    char csv_path[PATH_MAX];
    if(!dmr_qcut_resolve_labels_csv(job_dir, csv_path, sizeof(csv_path)) || csv_path[0] == '\0')
        return 0;

    FILE *fp = fopen(csv_path, "r");
    if(!fp) return 0;

    char line[4096];

    if(!fgets(line, sizeof(line), fp))
    {
        fclose(fp);
        return 0;
    }

    int col_job_id       = dmr_qcut_find_col_idx(line, "job_id");
    int col_subcircuit   = dmr_qcut_find_col_idx(line, "subcircuit");
    int col_label        = dmr_qcut_find_col_idx(line, "label");
    int col_num_qubits   = dmr_qcut_find_col_idx(line, "num_qubits");
    int col_depth        = dmr_qcut_find_col_idx(line, "depth");
    int col_twoq         = dmr_qcut_find_col_idx(line, "two_qubit_gates");
    int col_source_json  = dmr_qcut_find_col_idx(line, "source_json");

    if(col_subcircuit < 0 || col_label < 0)
    {
        fclose(fp);
        return 0;
    }

    int cap = 32;
    int used = 0;
    DMRQCutRow *rows = (DMRQCutRow*)calloc((size_t)cap, sizeof(DMRQCutRow));
    if(!rows)
    {
        fclose(fp);
        return 0;
    }

    while(fgets(line, sizeof(line), fp))
    {
        dmr_trim_inplace(line);
        if(line[0] == '\0') continue;

        char tok_sub[256] = {0};
        char tok_lab[64]  = {0};

        if(!dmr_qcut_csv_token_n(line, col_subcircuit, tok_sub, sizeof(tok_sub))) continue;
        if(!dmr_qcut_csv_token_n(line, col_label, tok_lab, sizeof(tok_lab))) continue;
        if(dmr_qcut_is_dup_subcircuit(rows, used, tok_sub)) continue;

        if(used >= cap)
        {
            cap *= 2;
            DMRQCutRow *nr = (DMRQCutRow*)realloc(rows, (size_t)cap * sizeof(DMRQCutRow));
            if(!nr)
            {
                free(rows);
                fclose(fp);
                return 0;
            }
            rows = nr;
            memset(rows + used, 0, (size_t)(cap - used) * sizeof(DMRQCutRow));
        }

        // Fill row
        snprintf(rows[used].subcircuit, sizeof(rows[used].subcircuit), "%s", tok_sub);
        snprintf(rows[used].label, sizeof(rows[used].label), "%s", tok_lab);

        char tmp[256];

        if(col_job_id >= 0 && dmr_qcut_csv_token_n(line, col_job_id, tmp, sizeof(tmp)))
            snprintf(rows[used].job_id, sizeof(rows[used].job_id), "%s", tmp);

        if(col_num_qubits >= 0 && dmr_qcut_csv_token_n(line, col_num_qubits, tmp, sizeof(tmp)))
            rows[used].num_qubits = atoi(tmp);

        if(col_depth >= 0 && dmr_qcut_csv_token_n(line, col_depth, tmp, sizeof(tmp)))
            rows[used].depth = atoi(tmp);

        if(col_twoq >= 0 && dmr_qcut_csv_token_n(line, col_twoq, tmp, sizeof(tmp)))
            rows[used].two_qubit_gates = atoi(tmp);

        if(col_source_json >= 0 && dmr_qcut_csv_token_n(line, col_source_json, tmp, sizeof(tmp)))
            snprintf(rows[used].source_json, sizeof(rows[used].source_json), "%s", tmp);

        used++;
    }

    fclose(fp);

    *out_rows = rows;
    *out_n = used;
    return 1;
}

/**
 * @brief Free label rows previously returned by dmr_qcut_read_labels_rows.
 *
 * @param rows Rows array (may be NULL).
 */
void dmr_qcut_free_labels_rows(DMRQCutRow *rows)
{
    free(rows);
}

/**
 * @brief Write routing decisions to a per-iteration CSV file.
 *
 * Creates <job_dir>/dmr_routes_iter_<iter>.csv and writes one row per subcircuit.
 *
 * @param job_dir Base directory for the QCUT job.
 * @param job_id_str Optional job id override (if NULL/empty, row job_id is used).
 * @param rows Label rows previously read (length @p n).
 * @param n Number of rows.
 * @param routes Array of route strings (length @p n).
 * @param reasons Array of reason strings (length @p n).
 * @param iter Iteration index to encode in the output filename and first column.
 * @return 1 on success, 0 on failure.
 */
int dmr_qcut_write_routes_csv(const char *job_dir,
                             const char *job_id_str,
                             const DMRQCutRow *rows,
                             int n,
                             const char *const *routes,
                             const char *const *reasons,
                             int iter)
{
    if(!job_dir || !rows || n <= 0 || !routes || !reasons) return 0;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/dmr_routes_iter_%d.csv", job_dir, iter);

    FILE *fp = fopen(path, "w");
    if(!fp) return 0;

    fprintf(fp, "iter,job_id,subcircuit,orig_label,route,reason,num_qubits,depth,two_qubit_gates,source_json\n");

    for(int i=0;i<n;i++)
    {
        const char *jid = (job_id_str && job_id_str[0]) ? job_id_str : rows[i].job_id;
        fprintf(fp, "%d,%s,%s,%s,%s,%s,%d,%d,%d,%s\n",
                iter,
                jid ? jid : "",
                rows[i].subcircuit,
                rows[i].label,
                routes[i] ? routes[i] : "",
                reasons[i] ? reasons[i] : "",
                rows[i].num_qubits,
                rows[i].depth,
                rows[i].two_qubit_gates,
                rows[i].source_json);
    }

    fclose(fp);
    return 1;
}
