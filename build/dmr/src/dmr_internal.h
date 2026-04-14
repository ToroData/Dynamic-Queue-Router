/**
 * @file dmr_internal.h
 * @brief DMR internal functions and constants
 *
 * Contains definitions for internal functions and
 * constants used within DMR. Not intended for external use.
 */

#ifndef DMR_INTERNAL_H
#define DMR_INTERNAL_H

// For asprintf and struct addrinfo
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <slurm/slurm.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "../extern/uthash.h"

// Newer versions of Slurm migrated the version number logic to new file
#ifndef SLURM_VERSION_NUMBER
#include <slurm/slurm_version.h>
#endif

// For use of TALP-related policies
#ifdef COMPILED_WITH_TALP
#include "dlb.h"
#include "dlb_talp.h"
#endif

// Used to turn a #define VAR to string literal "VAR"
#define NAMEOF(x) #x

/** @brief In-memory representation of one row in <job_dir>/dmr_labels.csv. */
typedef struct
{
    char job_id[64];         // as string
    char subcircuit[256];    // e.g., "frag_000.meta"
    char label[32];          // "HPC" | "QC" | "Undefined"
    int num_qubits;
    int depth;
    int two_qubit_gates;
    char source_json[256];   // e.g., "frag_000.meta.json"
} DMRQCutRow;

/**
 * @brief Load QCut labeling rows from <job_dir>/dmr_labels.csv.
 *
 * @param[in]  job_dir   Directory containing dmr_labels.csv.
 * @param[out] out_rows  On success, pointer to malloc'd array of rows.
 * @param[out] out_n     Number of rows parsed.
 *
 * @return 1 on success, 0 on error (file missing, parse failure, OOM).
 */
int dmr_qcut_read_labels_rows(const char *job_dir, DMRQCutRow **out_rows, int *out_n);

/**
 * @brief Emit a routing decision CSV for a given iteration.
 *
 * @param[in] job_dir     Base directory for output file.
 * @param[in] job_id_str  Optional job identifier string (traceability).
 * @param[in] rows        Array of labeling rows.
 * @param[in] n           Number of rows.
 * @param[in] routes      Array of chosen routes per row ("HPC"/"QC"/...).
 * @param[in] reasons     Array of decision reasons per row.
 * @param[in] iter        Reconfiguration iteration number.
 *
 * @return 1 on success, 0 on error (I/O failure, invalid args).
 *
 * @note This function does not modify DQR or DMR state directly.
 *       It only emits routing metadata for consumption by later stages.
 */
int dmr_qcut_write_routes_csv(const char *job_dir,
                             const char *job_id_str,
                             const DMRQCutRow *rows,
                             int n,
                             const char *const *routes,
                             const char *const *reasons,
                             int iter);


/**
 * @def PROC_ZERO
 * @brief Used whenever referring to process 0
 */
#define PROC_ZERO 0

/**
 * @def NULLCHAR_LEN
 * @brief Length of the nullchar (1 byte)
 */
#define NULLCHAR_LEN 1

/**
 * @def MINUTE_IN_SECONDS
 * @brief A minute converted to seconds
 */
#define MINUTE_IN_SECONDS 60

/**
 * @def DEFAULT_SLURM_JOBID
 * @brief Placeholder Slurm job ID in cases where none exists
 */
#define DEFAULT_SLURM_JOBID UINT32_MAX

/**
 * @def FILE_PATH_MAX
 * @brief Maximum space (bytes) to allocate for file paths
 */
#define FILE_PATH_MAX 4096

/**
 * @def MAX_EXPANSIONS
 * @brief Maximum number of expansions to allow at once
 */
#define MAX_EXPANSIONS 512

/**
 * @def SLURM_JOBID_ENVAR
 * @brief The name of the environment variable (determined by Slurm) which gives the current job id
 */
#define SLURM_JOBID_ENVAR "SLURM_JOBID"

/**
 * @def SLURM_USER_ENVAR
 * @brief The name of the environment variable (determined by Slurm) which gives the current Slurm username
 */
#define SLURM_USER_ENVAR "SLURM_JOB_USER"

/**
 * @def SLURM_ACCOUNT_ENVAR
 * @brief The name of the environment variable (determined by Slurm) which gives the current Slurm account
 */
#define SLURM_ACCOUNT_ENVAR "SLURM_JOB_ACCOUNT"

/**
 * @def DMR_SLURM_JOB_ACCOUNT
 * @brief The name of the environment variable which can be used to override the value read from Slurm
 */
#define DMR_SLURM_JOB_ACCOUNT "DMR_SLURM_JOB_ACCOUNT"

/**
 * @def SLURM_PARTITION_ENVAR
 * @brief The name of the environment variable (determined by Slurm) which gives the current Slurm partition
 */
#define SLURM_PARTITION_ENVAR "SLURM_JOB_PARTITION"

/**
 * @def DMR_SLURM_JOB_PARTITION
 * @brief The name of the environment variable which can be used to override the value read from Slurm
 */
#define DMR_SLURM_JOB_PARTITION "DMR_SLURM_JOB_PARTITION"

/**
 * @def SLURM_QOS_ENVAR
 * @brief The name of the environment variable (determined by Slurm) which gives the current Slurm QoS
 */
#define SLURM_QOS_ENVAR "SLURM_JOB_QOS"

/**
 * @def DMR_SLURM_JOB_QOS
 * @brief The name of the environment variable which can be used to override the value read from Slurm
 */
#define DMR_SLURM_JOB_QOS "DMR_SLURM_JOB_QOS"

/**
 * @def EXPANSION_ENVAR
 * @brief The name of the environment variable to store the current number of expansions in
 *
 * @warning This will need to match the value in dmr_wrapper
 *
 */
#define EXPANSION_ENVAR "DMR_EXPANSION_COUNT"

/**
 * @def DMR_STATE_FILE_ENVAR
 * @brief The name of the environment variable to store DMR state for checkpoint/restart
 *
 * @warning This will need to match the value in dmr_wrapper
 */
#define DMR_STATE_FILE_ENVAR "DMR_STATE_FILE"

/**
 * @def DMR_RECONFIG_COUNT_ENVAR
 * @brief The name of the environment which indicates how many times DMR has reconfigured
 *
 * @warning This will need to match the value in dmr_wrapper
 */
#define DMR_RECONFIG_COUNT_ENVAR "DMR_RECONFIG_COUNT"

/**
 * @def DMR_RECONFIG_TIME_ENVAR
 * @brief Time at which the last reconfiguration time was started, for analytics
 *
 * @warning This will need to match the value in dmr_wrapper
 */
#define DMR_RECONFIG_TIME_ENVAR "DMR_RECONFIG_TIME"

/**
 * @def ENVAR_FORMAT_UINT32T
 * @brief The formatting string for printing environment variables containing uint32_t values
 */
#define ENVAR_FORMAT_UINT32T "%s=%" PRIu32

/**
 * @def ENVAR_FORMAT_CHARARR
 * @brief The formatting string for printing environment variables containing string (char array) values
 */
#define ENVAR_FORMAT_CHARARR "%s=%s"

/**
 * @def ENVAR_FORMAT_DOUBLE
 * @brief The formatting string for printing environment variables containing double precision values
 */
#define ENVAR_FORMAT_DOUBLE "%s=%f"

/**
 * @def ENVAR_FORMAT_DOUBLE
 * @brief The formatting string for printing environment variables containing integer values
 */
#define ENVAR_FORMAT_INT "%s=%d"

/**
 * @def EXPAND_JOBNAME_MAIN
 * @brief The main/shared part of the name formatting string to give expander jobs
 *
 * Expander jobs will have a format such as DMREXPAND_1234567 where 1234567 is a uint32_t
 * Since there may be many expander jobs with the same parent, a suffix is added; see EXPAND_JOBNAME_SUFFIX
 */
#define EXPAND_JOBNAME_MAIN "DMREXPAND_%" PRIu32

/**
 * @def EXPAND_JOBNAME_SUFFIX
 * @brief Formatting string for the suffix to append to the main jobname as defined by EXPAND_JOBNAME_MAIN
 */
#define EXPAND_JOBNAME_SUFFIX "_%d"

/**
 * @def DMR_ERROR_PREFIX
 * @brief The prefix to print in front of DMR error messages
 */
#define DMR_ERROR_PREFIX "DMR Error: "

/**
 * @def EXPAND_JOB_OUTPUT_PATH
 * @brief Where to direct the output of the expander jobs
 *
 * This output is unlikely to provide much helpful data, so it is discarded by default
 */
#define EXPAND_JOB_OUTPUT_PATH "/dev/null"

/**
 * @def INTEGERS_FROM_ROOT_COUNT
 * @brief Number of integer data items to broadcast from root when initializing
 */
#define INTEGERS_FROM_ROOT_COUNT 1

/**
 * @def TYPES_FROM_ROOT_COUNT
 * @brief Total number of datatypes to broadcast from root when initializing
 */
#define TYPES_FROM_ROOT_COUNT 1

/**
 * @def BLOCKING_REQ_SLEEPTIME_S
 * @brief How long to sleep when doing blocking waits to acquire resources
 * 
 * This cooldown is to avoid overloading the Slurm controller
 */
#define BLOCKING_REQ_SLEEPTIME_S 1

/**
 * @def DLB_MONITOR_NAME_ACC
 * @brief What to name the accumulated (not reset each time) TALP monitor, if compiled with TALP
 */
#define DLB_MONITOR_NAME_ACC "DMRTalpMonitorAcc"

/**
 * @def DLB_MONITOR_NAME_INS
 * @brief What to name the instantaneous (reset each time) TALP monitor, if compiled with TALP
 */
#define DLB_MONITOR_NAME_INS "DMRTalpMonitorIns"

/**
 * Compatibility wrapper for SLURM hostlist_t
 */
#if SLURM_VERSION_NUMBER < SLURM_VERSION_NUM(23, 11, 0)

/**
 * @brief Legacy SLURM hostlist_t format
 */
typedef hostlist_t compat_hostlist_t;

#else

/**
 * @brief New SLURM hostlist_t format
 */
typedef hostlist_t *compat_hostlist_t;

#endif

/**
 * @brief Used internally to determine what course of action is appropriate at each stage
 */
typedef enum InternalActionEnum
{
    NONE,                  /**< DMR is not currently expecting any user triggered or external event  */
    EXPAND_WAIT_FOR_SLURM, /**< DMR is currently waiting for a Slurm expansion job to be ready */
    EXPAND_READY_RECONFIG, /**< A Slurm expansion job is ready, DMR is waiting for a call to dmr_reconfigure() */
    RESTARTED_NEED_DATA,   /**< Restarted because of an expand/shrink; need to get some data to work with */
    SHRINKING,             /**< Waiting for user to transfer data and dmr_reconfigure() to shrink */
    SHRINKING_FINALIZING,  /**< Waiting for the library user to call dmr_finalize() to complete shrink */
    EXPANDED_FINALIZING    /**< A MPI_Comm_spawn has reconfigured the world to grow; current process needs to dmr_finalize() */
} InternalAction;

/**
 * @brief Enum to carry information about results from expansion request
 */
typedef enum ExpandResultEnum
{
    EXPAND_SUCCESS, /**< Outcome of SLURM expander job if determined to be a success  */
    EXPAND_FAILED,  /**< Outcome of SLURM expander job if determined to be a failure  */
    EXPAND_PENDING  /**< Outcome of SLURM expander job if undertermined  */
} ExpandResult;

/**
 * @brief Enum to indicate which level of output to print (debug and analytics)
 */
enum DMR_OUTPUT_LEVEL_OPTIONS
{
    OUTPUT_LEVEL_UNKNOWN = INT32_MIN, /**< Placeholder before caching the value */
    NEVER_PRINT_OUTPUT = 0,           /**< Level 0 or below: never print output */
    PRINT_FROM_PROC_ZERO = 1,         /**< Level 1: print if we can determine we are on process 0 */
    PRINT_ANYTIME = 2                 /**< Level 2 or above: print whenever called */
};

/**
 * @brief Information recorded about running Slurm jobs
 */
typedef struct DMRSlurmJobInfoStruct
{
    uint32_t job_id;            /**< SLURM job ID  */
    int host_count;             /**< Number of hosts in SLURM job  */
    compat_hostlist_t hostlist; /**< List of hosts in SLURM job  */
    int *proc_counts;           /**< Array of process counts per host in hostlist, in same order  */
    int total_procs;            /**< Total number of processes in the hosts of the SLURM job  */
    bool should_kill;           /**< Whether or not this SLURM job is due to be killed due to a prior reconfiguration  */
    bool should_shrink;         /**< Whether or not this SLURM job is due to be shrunk due to a prior reconfiguration  */
} DMRSlurmJobInfo;

/**
 * @brief The state of any given MPI process running DMR
 */
typedef struct DMRStateStruct
{
    bool dmr_is_initialized;                  /**< Whether or not DMR is currently initialized  */
    bool dmr_soft_crash;                      /**< Whether some error has occurred which has silently disabled DMR functionality */
    bool is_root_process;                     /**< Whether this process is the manager of its MPI_COMM_WORLD */
    int local_comm_rank;                      /**< This process's rank in its MPI_COMM_WORLD */
    int local_comm_size;                      /**< This process's MPI_COMM_WORLD size */
    InternalAction action_in_progress;        /**< What action DMR is expecting the user to take */
    int expansion_status;                     /**< Current status of a pending expansion job, converted from ExpandResult type */
    MPI_Request *expansion_request;           /**< MPI_Request to check for result of expansion job from controller process */
    MPI_Comm INTERNAL_COMM_WORLD;             /**< Copy of MPI_COMM_WORLD, to avoid non-library MPI operations interfering with our actions */
    int current_nodes;                        /**< How many nodes are part of the current MPI_COMM_WORLD */
    int reconf_step_inhibitor;                /**< Inhibitor to prevent reconfiguration steps from being taken */
    int current_reconf_step_inhibitor;        /**< Current value of the inhibitor, used to determine if it has been cleared */
    int policy_min_nodes;                     /**< Minimum number of nodes to reconfigure to when determined by policy */
    int policy_max_nodes;                     /**< Maximum number of nodes to reconfigure to when determined by policy  */
    int policy_stride;                        /**< Number of nodes to reconfigure in multiples of when determined by policy */
    int policy_pref_nodes;                    /**< Preferred number of nodes to reconfigure to when determined by policy */
} DMRState;

/* DQR opaque context defined in dqr.h */
typedef struct DQRContext DQRContext;
typedef struct DQRBackendSimState DQRBackendSimState;

/**
 * @brief The state of a controller MPI process (rank 0 in the communicator) in DMR
 */
typedef struct DMRControllerStateStruct
{
    time_t global_end_time;               /**< The time the initial Slurm job is scheduled to terminate */
    time_t expansion_check_time;          /**< The time the pending expander job's end time was last adjusted */
    compat_hostlist_t expanding_hostlist; /**< Slurm hostlist type containing all nodes due to be added to the DMR job */
    int new_node_count;                   /**< Nodes in the current nodelist, updated to reflect any deletions or additions */
    int arg_count;                        /**< Argument count (argc) passed to dmr during initialization */
    char **arg_array;                     /**< Argument array (argv) passed to dmr during initialization */
    char *executable_name;                /**< Name of the executable read from argv[0] */
    uint32_t main_jobid;                  /**< The initial Slurm job id */
    uint32_t slurm_userid;                /**< The user ID of the user who submitted the initial Slurm job */
    uint32_t expanding_jobid;             /**< The Slurm job id of the job which has been submitted as an expansion job */
    time_t expand_start_run;              /**< Time at which the current expander Slurm job started running */
    int nodes_in_next_expand;             /**< Number of nodes to include the next time we are expanding */
    int procs_in_next_expand;             /**< Exact number of processes to spawn the next time we are expanding, if set */
    int procs_per_node;                   /**< Processes per node in next expand, if set */
    DMRSlurmJobInfo *jobs_info;           /**< Information about all the Slurm jobs part of this DMR run, including original */
    int jobs_count_alive;                 /**< Number of jobs belonging to the current DMR iteration, stored in jobs_info */
    int jobs_count_dead;                  /**< Number of jobs stored in jobs_info after jobs_count_alive marked for termination */
    double reconfig_start_curr;           /**< Time the current reconfiguration started, if applicable */
    double reconfig_start_last;           /**< Time the last reconfiguration started, if applicable */
    int nodes_next_shrink;                /**< How many nodes would be removed if we shrunk now */
    int procs_next_shrink;                /**< How many processes would be removed if we shrunk now */

    DQRContext *dqr_ctx;                  /**< Persistent DQR context; owned by DMR root. */
} DMRControllerState;

/**
 * @brief Struct with information to broadcast to all processes when initializing
 */
typedef struct DMRInitialInfoStruct
{
    int int_data[INTEGERS_FROM_ROOT_COUNT]; /**< All integer data to broadcast */
} DMRInitialInfo;

/**
 * @brief Struct to load information about nodes and their processes into
 */
typedef struct NodeInfoStruct
{
    char node_name[MPI_MAX_PROCESSOR_NAME]; /**< The node's name from MPI_Get_processor_name(...)  */
    int proc_count;                         /**< The number of processes on this node  */
} NodeInfo;

/**
 * @brief Hashtable struct using uthash for node name to process count pairings
 */
typedef struct NodeHashTableStruct
{
    char *node_ip;     /**< IP address of node, or if IP lookup failed, just the node's name */
    int processes;     /**< Number of processes belonging to the node */
    UT_hash_handle hh; /**< Handle needed for uthash internals */
} NodeHashTable;

/**
 * @brief Struct to store information from TALP in, if applicable
 */
typedef struct TALPInfoStruct
{
    double communication_efficiency; /**< Communication efficiency metric  */
    float talp_etime;                /**< Elapsed time metric  */
} TALPInfo;


/**
 *************************************************************
 * @section dmr_core.c
 * Core functionality for DMR setup and reconfigurations
 *************************************************************
 */

void spawn_mpi_world(DMRState *dmr_state, DMRControllerState *controller_state);

bool checkpoint_dmr_state(DMRState *dmr_state, DMRControllerState *controller_state);

bool manage_pending_job(DMRControllerState *controller_state);

bool broadcast_expander_status(DMRState *dmr_state, DMRControllerState *controller_state, bool ready_to_proceed);

bool query_expand_result(DMRState *dmr_state);

bool identify_nodes(DMRState *dmr_state, int *number_of_nodes, NodeInfo **node_data);

/**
*************************************************************
* @section dmr_policy.c
* Code relating to processing policies to reconfigure, provided as DMRSuggestions
*************************************************************
*/

DMRSuggestion process_if_policy(DMRState *dmr_state, DMRControllerState *controller_state, DMRSuggestion original_suggestion);

void register_talp_regions();

/**
*************************************************************
* @section dmr_slurm.c
* Logic used to interact directly with the Slurm API for DMR
*************************************************************
*/

#if SLURM_VERSION_NUMBER < SLURM_VERSION_NUM(20, 11, 0)

void slurm_init(const char *conf);

void slurm_fini(void);

#endif

time_t get_start_run_time(uint32_t job_id);

bool add_all_hosts(compat_hostlist_t the_hostlist, uint32_t job_id, int *list_count);

bool sync_with_slurm(DMRControllerState *controller_state, NodeHashTable *node_hashtable, bool is_original_execution);

void get_job_state(uint32_t job_id, enum job_states *state);

bool kill_job(uint32_t job_id);

int get_job_node_count(uint32_t job_id);

bool merge_expander_job(DMRControllerState *controller_state);

bool jobs_can_grow(void);

bool jobs_can_shrink(void);

compat_hostlist_t merge_all_hostlists(DMRControllerState *controller_state);

void slurm_dmr_reconfigure(DMRControllerState *controller_state);

bool adjust_pending_job_time(DMRControllerState *controller_state);

void expand_slurm_world(DMRState *dmr_state, DMRControllerState *controller_state);

void hostlist_destroy_set_null(compat_hostlist_t *the_hostlist);

/**
*************************************************************
* @section dmr_utils.c
* Helper functions used by the main DMR API definitions
*************************************************************
*/

void dmr_output(const char *format, ...);

void dmr_analytics(DMRState *dmr_state, DMRControllerState *controller_state, double current_time, double communication_efficiency, char const *function, char const *state, bool print_reconfig_time);

void debug_output(const char *format, ...);

void dmr_error(const char *format, ...);

void dmr_abort(void);

char const *suggestion_to_string(DMRSuggestion suggestion);

char const *internal_action_to_string(InternalAction action);

int get_minutes_left(time_t end_time);

void init_dmr_state(DMRState **dmr_state);

void free_dmr_state(DMRState *dmr_state);

void init_controller_state(DMRControllerState **controller_state);

void free_controller_state(DMRControllerState *controller_state);

void free_dmr_intercomm(char *side);

bool check_dmr_initialized(DMRState *dmr_state, bool *print_error);

bool get_uint32_from_env(char *environment_var, uint32_t *the_number);

bool get_double_from_env(char *environment_var, double *the_number);

void get_counts_post_reconfig(DMRState *dmr_state, DMRControllerState *controller_state, int *new_expansions, int *new_reconfigs);

bool use_checkpoint_restart(void);

int get_procs_removed_in_shrink(DMRControllerState *controller_state, int nodes);

int get_nodes_removed_in_shrink(DMRControllerState *controller_state, int procs);

int get_proc_count_for_node(int total_procs, int total_nodes, int node_idx);

void set_existing_proc_counts(DMRControllerState *controller_state, int max_nodes, int *procs_per_node, int procs_in_reconfig, int *added_procs, int *added_nodes);

void create_info_datatype(MPI_Datatype *info_datatype);

int get_default_nodes_in_shrink(void);

bool requests_are_blocking(void);

bool can_remove_resources(DMRState *dmr_state);

bool should_print_analytics(void);

double get_global_time(void);

void create_nodeinfo_datatype(MPI_Datatype *the_datatype);

char *hostname_to_ip(const char *hostname);

void *malloc_or_abort(size_t size);

char *strdup_or_abort(const char *string);

void asprintf_or_abort(char **output, const char *format, ...);

void asprintf_or_null(char **output, const char *format, ...);

void dmr_dlb_finalize(void);

int get_nearest_jobs_to_nodes(DMRControllerState *controller_state, int nodes_to_remove);

void internal_set_jobs_next_shrink(DMRControllerState *controller_state, int jobs);

/**
*************************************************************
* @section talp_hook.c
* Code integrating with DLB's TALP, if compiled with it
*************************************************************
*/

void print_talp_CE_ins(DMRState *dmr_state, DMRControllerState *controller_state);

void print_talp_CE_acc(DMRState *dmr_state, DMRControllerState *controller_state);

TALPInfo get_talp_info_acc(DMRState *dmr_state, DMRControllerState *controller_state);

bool talp_enabled(void);

void start_monitoring_regions(DMRState *dmr_state);

void stop_monitoring_regions(void);

void stop_and_finalize_dlb(void);

#endif // DMR_INTERNAL_H