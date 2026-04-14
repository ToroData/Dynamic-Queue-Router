/**
 * @file dmr.h
 * @brief DMR main include file
 *
 * Contains the main functionality of DMR to call when using the library
 * Include this file to use the library's functionality.
 */

#if !defined(DMR_H)
#define DMR_H

// Ensure compatibility with C++ code
#if defined(__cplusplus)
extern "C" {
#endif

#include "dqr.h"
#include <mpi.h>
#include <stdint.h>


/**
 * @def DMR_DEBUG_LEVEL
 * @brief Set this variable to display debug prints.
 * 
 * Level 0 or below: never print debug statements
 * Level 1: print debug statements only if we can determine that they eminate from MPI process 0
 * Level 2 or above: always print debug statements
 * 
 * Other levels: never print debug statements
 * 
 * @note You can set an environment variable of the same name when running to override the compiled value
 */
#if !defined(DMR_DEBUG_LEVEL)
#define DMR_DEBUG_LEVEL 0
#endif

/**
 * @def DMR_PRINT_ANALYTICS
 * @brief Whether or not to print analytics information
 * 
 * Whether or not to print analytics information at runtime containing the following at each initialization:
 * nodes, processes in current configuration, time since last reconfiguration
 * 
 * @note You can set an environment variable of the same name when running to override the compiled value
 */
#if !defined(DMR_PRINT_ANALYTICS)
#define DMR_PRINT_ANALYTICS 0
#endif

/**
 * @def DMR_NODES_IN_EXPAND
 * @brief The default number of nodes to add in each expand.
 *
 * How many nodes to add each time an expansion is requested by the DMR program.
 * This is one of three ways to adjust this value. The following priority is respected:
 *
 * @note The number of nodes is determined by the following (in order of priority):
 * - If `dmr_set_nodes_next_expand()` was called, its value is used.
 * - Else, if the `DMR_NODES_IN_EXPAND` environment variable is set, its value is used.
 * - Otherwise, the compile-time constant `NODES_IN_EXPAND` is used.
 */
#if !defined(DMR_NODES_IN_EXPAND)
#define DMR_NODES_IN_EXPAND 1
#endif

/**
 * @def DMR_PROCS_PER_NODE
 * @brief Default number of processes per node in expander jobs.
 *
 * Defines how many processes to spawn for each node in expander jobs.
 * This is one of three ways to adjust this value. The following priority is respected:
 *
 * @note The number of processes per node is determined by the following (in order of priority):
 * - If `dmr_set_ppn_next_expand()` was called, its value is used.
 * - Else, if the `DMR_PROCS_PER_NODE` environment variable is set, its value is used.
 * - Otherwise, the compile-time constant `DMR_PROCS_PER_NODE` is used.
 */
#if !defined( DMR_PROCS_PER_NODE)
#define DMR_PROCS_PER_NODE 1
#endif

/**
 * @def DMR_NODES_IN_SHRINK
 * @brief Default number of nodes to shrink by, if this functionality is available.
 *
 * Sets how many nodes to remove each time a shrink operation is requested by default.
 * This will only work if node removal is supported on the system and DMR is compiled
 * with `JOBS_CAN_SHRINK` set to 1. If the default value exceeds the current number of nodes,
 * it will be adjusted so that the resulting configuration has at least one node.
 *
 * @note The number of nodes to shrink by is determined by the following (in order of priority):
 * - If `dmr_set_nodes_next_shrink()` was called, its value is used.
 * - Else, if the `DMR_NODES_IN_SHRINK` environment variable is set, its value is used.
 * - Otherwise, the compile-time constant `DMR_NODES_IN_SHRINK` is used.
 */
#if !defined(DMR_NODES_IN_SHRINK)
#define DMR_NODES_IN_SHRINK 1
#endif


/**
 * @def DMR_BLOCKING_REQ
 * @brief Whether or not to block while waiting for resources
 * 
 * If DMR_BLOCKING_REQ is enabled, dmr_check() will run indefinitely
 * until any requested resources have been acquired (or the acquisition failed).
 * This may be useful if you are running in an environment in which the resources 
 * are very likely to be available, such as Slurm4DMR.
 */
#if !defined(DMR_BLOCKING_REQ)
#define DMR_BLOCKING_REQ 0
#endif

/**
 * @def CUSTOM_SLURM_BIN_PREFIX
 * @brief Path to find Slurm executables
 * 
 * Custom path for expander jobs to look for the scontrol
 * executable. Is needed if the required option is not on PATH.
 */
#if !defined(CUSTOM_SLURM_BIN_PREFIX)
#define CUSTOM_SLURM_BIN_PREFIX ""
#endif

/**
 * @def ENFORCE_SYNCHRONIZATION
 * @brief Whether or not to enforce synchronization when expanding
 * 
 * If this setting is disabled, then DMR will not make sure the processes
 * all agree on the DMRAction returned from dmr_check when expanding. This
 * may lead to non-root processes completing more iterations than the root.
 * If this is not an issue for you, you might gain a performance benefit 
 * from disabling synchronization.
 */
#if !defined(ENFORCE_SYNCHRONIZATION)
#define ENFORCE_SYNCHRONIZATION 1
#endif

/**
 * @def CHECKPOINT_RESTART
 * @brief Whether or not to use checkpoint-restart for shrinking
 * 
 * Without checkpoint-restart, a custom version of PRRTE is needed which
 * supports the removal of daemons. Alternatively, a non-aggressive Slurm
 * configuration which does not kill the SSH connection to nodes that leave
 * the allocation will also work
 */
#if !defined(CHECKPOINT_RESTART)
#define CHECKPOINT_RESTART 1
#endif

/**
 * @def WAIT_FOR_SLURM_S
 * @brief Time, in seconds, to wait for Slurm nodes to complete startup after starting to run
 *
 * Used to wait after a Slurm job has started running until spawning onto it. This is useful
 * in cases where nodes take some additional time after allocation before they are able to accept
 * ssh connections
 */
#if !defined(WAIT_FOR_SLURM_S)
#define WAIT_FOR_SLURM_S 0
#endif 

/**
 * @def JOBS_CAN_SHRINK
 * @brief Whether or not we can manually adjust the sizes of jobs to shrink
 * 
 * Whether or not we have access to the Slurm feature of removing nodes from
 * jobs while they are running.
 */
#if !defined(JOBS_CAN_SHRINK)
#define JOBS_CAN_SHRINK 1
#endif

/**
 * @def JOBS_CAN_GROW
 * @brief Whether or not we are able to use the Slurm feature of expanding running jobs
 * 
 * Indicates whether or not we can submit an expander job which can be merged into the main
 * job. This requires legacy Slurm functionality to work. Requires job shrinking functionality
 * to be enabled.
 */
#if !defined(JOBS_CAN_GROW)
#define JOBS_CAN_GROW 0
#endif

/**
 * @def DMR_TALP_SENSITIVITY
 * @brief Sensitivity factor of DMR's performance-aware policy
 * 
 * A higher value causes more resources to be added/removed in response
 * to differences in communication efficiency w.r.t target efficiency
 */
#if !defined(DMR_TALP_SENSITIVITY)
#define DMR_TALP_SENSITIVITY 15
#endif

/**
 * @def DMR_TALP_TARGET_CE
 * @brief The target communication efficiency to aim for in the performance-aware policy
 * 
 * Used when DMR's performance-aware policy is active. If the real value is found to be
 * greater, the world will expand. If less, the world will shrink.
 */
#if !defined(DMR_TALP_TARGET_CE)
#define DMR_TALP_TARGET_CE 0.8
#endif

/**
 * @brief Used by library user to suggest to DMR which action to take
 *
 * Tell DMR how to reconfigure, or to just stay in current configuration.
 * The suggestion will immediately be acted on if DMR is not currently performing some other action.
 */
typedef enum DMRSuggestionEnum
{
    ROUND_POLICY,           /**< Double node count up until dmr_get_policy_max_nodes(), then minimize */
    CE_POLICY,              /**< Aim for a communication efficiency of DMR_TALP_TARGET_CE */
    SLURM4DMR_ROUND_POLICY, /**< Expand up until dmr_get_policy_max_nodes(), then minimize */
    SLURM4DMR_CE_POLICY,    /**< Aim for a communication efficiency of DMR_TALP_TARGET_CE, considering cluster status */
    SLURM4DMR_QUEUE_POLICY, /**< Try to stay at node preference but accept min and max nodes */
    SHOULD_EXPAND,          /**< Expand the world by dmr_get_nodes_next_expand() nodes */
    SHOULD_SHRINK,          /**< Shrink by dmr_get_nodes_next_shrink() nodes */
    SHOULD_STAY,             /**< Do nothing */
    ROUTE_UNDEF_STEWARDSHIP, /**< Conservative routing recommendation for "Undefined" fragments */
    QCUT_PIPELINE_BALANCE,   /**< QCut pipeline balance policy */
} DMRSuggestion;

/**
 * @brief Used by DMR to tell user which action to take on current process
 *
 * Returned by the main dmr functions, indicating what step(s) the library user
 * should take next to ensure correct functionality of the library.
 */
typedef enum DMRActionEnum
{
    DMR_NO_ACTION,       /**< No action is required from the library user */
    DMR_RECONF,          /**< Should call dmr_reconfigure() */
    DMR_RESTART_RECONF,  /**< Should call restart logic to retrieve data, then call dmr_reconfigure() */
    DMR_REDIST_FINALIZE, /**< Should call checkpoint or data redistribution logic to save data, then call dmr_finalize() */
    DMR_FINALIZE,        /**< Should call dmr_finalize() */
    DMR_CLEANUP,         /**< Optionally call internal cleanup logic */
} DMRAction;

/**
 * @brief Status codes returned by @ref dmr_qcut_step.
 *
 * One non-blocking planning step for QCut and DQR integration.
 */
typedef enum
{
    /** No further work is pending (no PENDING/DISPATCHED fragments). */
    DMR_QCUT_STEP_DONE = 0,

    /** A dispatch wave was produced (>=1 fragment planned for HPC or QC). */
    DMR_QCUT_STEP_HAS_WORK = 1,

    /** Work exists, but nothing dispatchable in this step (e.g., capacity exhausted / waiting ACK). */
    DMR_QCUT_STEP_BLOCKED = 2,

    /** Error (invalid state, missing directory, I/O failure, etc.). */
    DMR_QCUT_STEP_ERROR = -1
} DMRQCutStepStatus;


/**
 * @brief The intercommunicator used for data reconfiguration
 *
 * The intercommunicator connecting the processes of each DMR reconfiguration,
 * enabling redistribution of data over MPI functionality.
 *
 * Not to be used unless suggested to by a DMRAction (DMR_RESTART_RECONF or REDIST_FINALIZE).
 * A helper function, dmr_intercomm_available(), is provided.
 */
extern MPI_Comm DMR_INTERCOMM;

/**
 * @brief Initialize the DMR library, unlocking other functionality
 *
 * Program starting point. Contains validation checks and initializers
 * ensuring that the library will work as intended. Take argc and argv
 * exactly as they exist in the main function of any C program.
 *
 * @param[in] argc Number of arguments in the argv array
 * @param[in] argv Array of arguments to pass on to any child processes (DMR assumes argv[0] is the executable)
 *
 * @return A DMRAction enum value for the user to act on. Possible return values for dmr_init are:
 *
 * - `DMR_NO_ACTION`: returned when DMR is initialized for the first time, or in error cases
 * - `DMR_RESTART_RECONFIG`: returned when DMR is initialized after having expanded or shrunk the communicator
 *
 * See the DMRAction definition for an explanation of the meaning of each action.
 */
DMRAction dmr_init(int argc, char *argv[]);

/**
 * @brief Suggest or check on DMR reconfiguration progress
 *
 * Call this function periodically to suggest a reconfiguration or check
 * on reconfiguration progress. Takes a DMRSuggestion value, which determines
 * how DMR will try to reconfigure the world.
 *
 * @param[in] suggested_reconfiguration A suggestion to DMR as to how to reconfigure processes. See DMRSuggestion definition.
 *
 * @pre All other processes must also call the function
 * @pre Suggestions must not differ between processes
 * @pre DMR has already been initialized with dmr_init()
 * @pre DMR has not already been finalized with dmr_finalize()
 * @pre DMR is not expecting some other action, as indicated by a returned DMRAction
 *
 * @return A DMRAction enum value for the user to act on. Possible return values for dmr_check are:
 *
 * - `DMR_NO_ACTION`: returned when no further action is possible or desired at this time, or in error cases
 * - `DMR_RECONFIG`: returned when the environment is ready to be expanded or shrunk
 *
 * See the DMRAction definition for an explanation of the meaning of each action.
 */
DMRAction dmr_check(DMRSuggestion suggested_reconfiguration);

/**
 * @brief Reconfigure the DMR environment
 *
 * Call this function only when it is suggested by dmr_initialize() or dmr_check().
 * Reconfigure the MPI processes as necessary for the action in progress.
 *
 * @pre DMR has already been initialized with dmr_init()
 * @pre DMR has not already been finalized with dmr_finalize()
 * @pre DMR is expecting a call to reconfigure, as indicated by a returned DMRAction
 *
 * @return A DMRAction enum value for the user to act on. Possible return values for dmr_reconfigure are:
 *
 * - `DMR_NO_ACTION`: returned by reconfigured processes when no further action is needed or in error cases
 * - `DMR_REDIST_FINALIZE`: returned on the communicator that is due to terminate when completing reconfiguration
 *
 * See the DMRAction definition for an explanation of the meaning of each action.
 */
DMRAction dmr_reconfigure();

/**
 * @brief Finalize the DMR environment
 *
 * Call this function only when it is suggested by dmr_check(), or when
 * finishing execution of the whole program. Clean up resources and, if necessary,
 * kill any pending expander job that is no longer needed. Exit process if we are reconfiguring.
 *
 * @warning Process termination point when called in the context of a reconfiguration
 *
 * @pre DMR has already been initialized with dmr_init()
 * @pre DMR has not already been finalized with dmr_finalize()
 *
 * @return A DMRAction enum value for the user to act on. Possible return values for dmr_finalize are:
 *
 * - `DMR_NO_ACTION`: returned in error cases
 * - `DMR_CLEANUP`: returned when called outside of a reconfiguration context
 * 
 * See the DMRAction definition for an explanation of the meaning of each action.
 */
DMRAction dmr_finalize();

/**
 * @brief One tick of the QCut policy runtime (rank 0 only).
 *
 * Advances completions, plans and commits the next wave using DQR, and dispatches
 * newly committed fragments to backends. Returns aggregate progress state.
 */
DMRQCutStepStatus dmr_qcut_step(const char *job_id_str);

/**
 * @brief Worker loop for ranks > 0.
 *
 * Blocks waiting for work messages from rank 0 and executes them, sending
 * completion messages back. This function returns only when rank 0 requests STOP.
 */
void dmr_qcut_worker_loop(void);

/**
 * @brief Rank 0 requests all workers to stop (best-effort).
 *
 * Use when the job is DONE or on fatal ERROR.
 */
void dmr_qcut_shutdown_workers(void);

/**
 * @brief ACK: fragment completed successfully (collective call).
 * @return 1 on success, 0 on failure.
 */
int dmr_qcut_ack_fragment_done(const char *frag_id);

/**
 * @brief ACK: fragment failed (collective call).
 * @param permanent 0 transient, 1 permanent
 * @return 1 on success, 0 on failure.
 */
int dmr_qcut_ack_fragment_failed(const char *frag_id, int permanent);

/**
 * @brief Check whether DMR is currently exposing a usable intercommunicator
 *
 * Check whether or not DMR_INTERCOMM is currently MPI_COMM_NULL or not.
 * In cases where we are expanding the world, DMR will expose an intercommunicator
 * which can be used to transfer data between the original and expanded world.
 * When shrinking, this will not be the case. This function can be used to determine
 * which of the two alternatives apply in the current case.
 *
 * @pre DMR has already been initialized with dmr_init()
 *
 * @return 1 if DMR_INTERCOMM is usable, or 0 otherwise
 */
int dmr_intercomm_available();

/**
 * @brief Get the number of expansion jobs that are currently active
 *
 * Get the number of expansion jobs currently active in DMR. The count
 * starts at 0, increments at every expansion, and decrements at every shrink.
 * This function is safe to call at any point of execution, even before dmr_init()
 * has been called.
 *
 * @return An integer representing the number of active expansion jobs, or -1 in case of failure
 */
int dmr_get_active_expansions();

/**
 * @brief Check how many times DMR has reconfigured itself since original launch
 *
 * Determines how many times since launch any resizing (shrinking or growing) has occurred,
 * from the perspective of the calling process. This function is safe to call at any point
 * of execution, even before dmr_init() has been called.
 *
 * @pre The environment variable DMR_RECONFIG_COUNT is set by the dmr_wrapper logic
 *
 * @return 0 if no expansions, a positive integer indicating the number of reconfigurations otherwise, or -1 in error cases
 */
int dmr_get_reconfig_count();

/**
 * @brief Get the number of nodes to request next time expanding
 *
 * Get the number of nodes to spawn onto the next time expanding. Only valid to call
 * from the root process.
 * 
 * @pre The calling process is the root process (rank 0)
 * @pre DMR has already been initialized with dmr_init() 
 *
 * @return The number of nodes to request next time expanding, or -1 in error cases
 */
int dmr_get_nodes_next_expand();

/**
 * @brief Set the number of nodes to request next time expanding
 *
 * Set the number of nodes to spawn onto the next time expanding. Only valid to call
 * from the root process before an expansion has been suggested. This value overrides
 * any value read from the compilation or environment, but resets after a reconfiguration
 * has occurred. See @ref DMR_NODES_IN_EXPAND for an explanation of the priority used.
 * 
 * @param[in] nodes Number of nodes to request
 * 
 * @pre The calling process is the root process (rank 0)
 * @pre DMR is not already in the process of expanding
 * @pre DMR has already been initialized with dmr_init() 
 */
void dmr_set_nodes_next_expand(int nodes);

/**
 * @brief Get the number of processes to spawn next time expanding
 *
 * Get the number of processes to spawn the next time expanding. Only valid to call
 * from the root process.
 * 
 * @pre The calling process is the root process (rank 0)
 * @pre DMR has already been initialized with dmr_init() 
 *
 * @return The number of processes to spawn next time expanding, or -1 in error cases
 */
int dmr_get_procs_next_expand();

/**
 * @brief Set the EXACT number of processes to spawn next time expanding
 *
 * Set the total number of processes to spawn the next time expanding. Only valid to call
 * from the root process before an expansion has been requested. The total number set
 * is divided between all the nodes in the next expand. Setting this overrides any
 * value set in dmr_set_ppn_next_expand.
 * 
 * @param[in] procs The number of processes to add
 * 
 * @pre The calling process is the root process (rank 0)
 * @pre DMR is not already in the process of expanding
 * @pre DMR has already been initialized with dmr_init() 
 */
void dmr_set_procs_next_expand(int procs);

/**
 * @brief Set the number of processes per node to spawn next time expanding
 *
 * Set the number of processes to spawn the next time expanding. Only valid to call
 * from the root process and before an expansion has been requested. This number will be multiplied
 * by the number of nodes in the next expand. Setting this will override any value
 * read from the environment or compilation default until the next reconfiguration.
 * See @ref DMR_PROCS_PER_NODE for an explanation of priority rules.
 * 
 * @param[in] procs Desired process per node count
 * 
 * @pre The calling process is the root process (rank 0)
 * @pre DMR is not already in the process of expanding
 * @pre DMR has already been initialized with dmr_init() 
 */
void dmr_set_ppn_next_expand(int ppn);

/**
 * @brief Get the number of nodes that would be removed if shrinking
 *
 * Get the number of nodes that would be removed from the program if a shrink
 * was completed now.
 * 
 * @pre Must be called from process 0
 * @pre DMR has already been initialized with dmr_init() 
 */
int dmr_get_nodes_next_shrink();

/**
 * @brief Set the number of nodes to remove next time shrinking
 *
 * Set the number of nodes to remove next time shrinking. This feature is
 * only available if we have the ability to shrink Slurm jobs. Calling this
 * function overrides any value read from the compilation or the environment.
 * See @ref NODES_IN_SHRINK for an explanation of priority rules.
 * 
 * @param[in] procs Desired nodes to shrink by
 * 
 * @pre DMR is compiled with JOBS_CAN_SHRINK == 1
 * @pre Calling process is process 0
 * @pre DMR has already been initialized with dmr_init() 
 */
void dmr_set_nodes_next_shrink(int nodes);

/**
 * @brief Set the number of jobs to remove next time shrinking
 * 
 * Set the count of jobs to remove when shrinking next time. This function should
 * be used when running in a configuration in which jobs themselves cannot shrink.
 * 
 * @param[in] procs Desired jobs to shrink by, which must be greater than 0 and less than or equal to @ref dmr_get_active_expansions
 * 
 * @pre DMR has already been initialized with dmr_init() 
 */
void dmr_set_jobs_next_shrink(int jobs);

/**
 * @brief Get the number of processes that would be removed if shrinking
 *
 * Get the number of processes that would be removed from the program if a shrink
 * was requested now.
 * 
 * @pre If compiled with JOBS_CAN_SHRINK == 1, then must be called from process 0
 * @pre DMR has already been initialized with dmr_init() 
 */
int dmr_get_procs_next_shrink();

/**
 * @brief Set the number of processes to remove next time shrinking
 *
 * Set the number of processes to remove next time shrinking. This feature is
 * only available if we have the ability to shrink Slurm jobs. Changing the
 * process count to remove will affect the node count which we are removing
 * in accorance with the new process count.
 * 
 * @param[in] procs Desired processes to shrink by
 * 
 * @pre DMR is compiled with JOBS_CAN_SHRINK == 1
 * @pre Calling process is process 0
 * @pre DMR has already been initialized with dmr_init() 
 */
void dmr_set_procs_next_shrink(int procs);

/**
 * @brief Get the number of nodes that are participating in the current DMR session
 * 
 * Get a count of the nodes that are part of the current configuration of DMR
 * 
 * @pre DMR has already been initialized with dmr_init() 
 */
int dmr_get_current_node_count();

/**
 * @brief Indicate whether an expansion job is currently pending
 * 
 * Get an indication of whether or not an expansion job is pending.
 * This can be used to cancel the job safely using dmr_cancel_expansion().
 * The return value will not change unless dmr reconfiguration logic is called
 * 
 * @pre DMR has already been initialized with dmr_init() 
 * 
 * @return 1 if an expansion is pending, or 0 otherwise
 */
int dmr_pending_expansion();

/**
 * @brief Cancel a pending expansion job
 * 
 * Cancel an expansion job that has been requested. This function MUST
 * be called by all the processes of the current DMR reconfiguration.
 * 
 * @pre All DMR processes in the current reconfiguration call the function
 * @pre DMR has already been initialized with dmr_init() 
 */
void dmr_cancel_expansion();

/**
 * @brief Set an inhibitor for requesting reconfigurations
 *
 * Set an inhibitor that will prevent DMR from requesting any new reconfiguration
 * when dmr_check is called. If the inhibitor is N, then one of every N calls to
 * dmr_check(...) are ignored. The inhibitor has no effect if a reconfiguration
 * is already underway.
 * 
 * @param[in] steps Value of the inhibitor
 * 
 * @pre DMR has already been initialized with dmr_init()
 * @pre ALL MPI processes in the current configuration call the function
 */
void dmr_set_reconf_step_inhibitor(int steps);

/**
 * @brief Set the minimum nodes when removing nodes by policy
 * 
 * Set the minimum number of nodes to scale to when auto-sizing due to policy.
 * Only applicable when a policy is in use which automatically adjust DMR program size. 
 * 
 * @param[in] nodes Minimum number of nodes
 *
 * @pre DMR has already been initialized with dmr_init()
 * @pre ALL MPI processes in the current configuration call the function
 */
void dmr_set_policy_min_nodes(int nodes);

/**
 * @brief Get the minimum nodes when removing nodes by policy
 * 
 * Set the minimum number of nodes to scale to when auto-sizing due to policy.
 * Only applicable when a policy is in use which automatically adjust DMR program size. 
 * 
 * @return Minimum number of nodes
 *
 * @pre DMR has already been initialized with dmr_init()
 */
int dmr_get_policy_min_nodes(void);

/**
 * @brief Set the maximum nodes when adding nodes by policy
 * 
 * Set the maximum number of nodes to scale to when auto-sizing due to policy.
 * Only applicable when a policy is in use which automatically adjust DMR program size. 
 * 
 * @param[in] nodes Maximum number of nodes
 *
 * @pre DMR has already been initialized with dmr_init()
 * @pre ALL MPI processes in the current configuration call the function
 */
void dmr_set_policy_max_nodes(int nodes);

/**
 * @brief Get the maximum nodes when adding nodes by policy
 * 
 * Get the maximum value currently in effect when resizing the DMR
 * program based on a policy.
 * 
 * @return Maximum number of nodes
 * 
 * @pre DMR has already been initialized with dmr_init()
 */
int dmr_get_policy_max_nodes(void);

/**
 * @brief Set the stride for applicable policies to use
 * 
 * Set the multiplier for policy-based decisions to use.
 * For example, if the multiplier is 2, expansions will double the size of the world
 * and shrinks will halve it.
 * 
 * @param[in] multiplier Value of the multiplier
 *
 * @pre DMR has already been initialized with dmr_init()
 * @pre ALL MPI processes in the current configuration call the function
 */
void dmr_set_policy_stride(int multiplier);

/**
 * @brief Get the stride that applicable policies use
 * 
 * Get the multiplier that was set for policy-based decisions to use.
 * 
 * @returns Value of the multiplier. The default is 2 before set by @ref dmr_set_policy_stride
 *
 * @pre DMR has already been initialized with dmr_init()
 */
int dmr_get_policy_stride(void);

/**
 * @brief Set the preferred number of nodes for applicable policies to use
 * 
 * Set the preferred number of nodes the current program should be in.
 * This will only have an effect for specific policies which can adjust to
 * match a preference
 * 
 * @param[in] nodes Preferred number of nodes
 *
 * @pre DMR has already been initialized with dmr_init()
 * @pre ALL MPI processes in the current configuration call the function
 */
void dmr_set_policy_pref_nodes(int nodes);

/**
 * @brief Get the preferred number of nodes for applicable policies to use
 * 
 * Get the preferred number of nodes the current program should be in, 
 * to be used by policies that utilize this value
 *  
 * @returns Preferred number of nodes
 *
 * @pre DMR has already been initialized with dmr_init()
 */
int dmr_get_policy_pref_nodes(void);

/**
 * @brief Label counts struct for QCut
 */
typedef struct DMRQCutLabelCounts
{
    int hpc;
    int qc;
    int undef;
    int total;
} DMRQCutLabelCounts;

/**
 * @brief Get label counts from QCut dmr_labels.csv in a job output directory.
 *
 * Resolution order for CSV path:
 *  1) DMR_QCUT_LABELS_FILE (absolute path)
 *  2) <job_dir>/dmr_labels.csv
 *
 * Expected CSV header contains "label" column. Typical row:
 *   job_id,subcircuit,label,num_qubits,depth,two_qubit_gates,source_json
 *
 * @param job_dir Path to job-x/output (can be NULL if DMR_QCUT_LABELS_FILE is set)
 * @param out_counts Output counts struct (required)
 * @return 1 on success, 0 on failure
 */
int dmr_qcut_get_label_counts(const char *job_dir, DMRQCutLabelCounts *out_counts);

/**
 * @brief Label QCut subcircuits for DMR consumption (QC/HPC/Undecided)
 *
 * Reads subcircuit artifacts under an output directory (default: ./output/subcircuits),
 * estimates cost/fidelity proxies using simple heuristics, and writes labeling files:
 *
 * - <output_dir>/dmr_labels.csv
 * - <output_dir>/subcircuits/<subcircuit>.dmr_label
 *
 * The labeling rules are configurable via:
 * - Compiled defaults in include/dmr_qcut_label.h
 * - Optional config file (DMR_QCUT_LABEL_CONFIG=/path/to/file)
 * - Environment overrides (e.g., DMR_QCUT_QC_MAX_QUBITS, etc.)
 *
 * @param[in] job_id Slurm job id (used only for traceability in outputs)
 *
 * @return 0 on success, non-zero on error
 */
int dmr_qcut_label_job(uint32_t job_id);


/**
 * @brief Macro to automatically deal with a DMRAction returned by a DMR function
 *
 * Simplifies the use of DMR through pre-provided responses to the possible actions
 * that are returned by DMR.
 *
 * @warning Possible program termination point
 *
 * @pre DMR has not already been finalized with dmr_finalize()
 *
 * @param[in] the_action A DMRAction returned by a DMR function
 * @param[in] redist_func The function to call to checkpoint or transfer data, or NULL if none needed at the point called
 * @param[in] restart_func The function to call to read or receive data when restarting, or NULL if none needed at the point called
 * @param[in] finalize_func The function to call to clean up resources, or NULL if none needed at the point called
 *
 */
#define DMR_AUTO(the_action, redist_func, restart_func, finalize_func)     \
    {                                                                      \
        switch (the_action)                                                \
        {                                                                  \
                                                                           \
        case DMR_NO_ACTION:                                                \
        {                                                                  \
            break;                                                         \
        }                                                                  \
                                                                           \
        case DMR_RECONF:                                                   \
        {                                                                  \
            if (dmr_reconfigure() == DMR_REDIST_FINALIZE)                  \
            {                                                              \
                redist_func;                                               \
                finalize_func;                                             \
                dmr_finalize(); /* Termination point */                    \
            }                                                              \
            break;                                                         \
        }                                                                  \
                                                                           \
        case DMR_RESTART_RECONF:                                           \
        {                                                                  \
            restart_func;                                                  \
            dmr_reconfigure();                                             \
            break;                                                         \
        }                                                                  \
                                                                           \
        case DMR_REDIST_FINALIZE:                                          \
        {                                                                  \
            redist_func;                                                   \
            finalize_func;                                                 \
            dmr_finalize(); /* Termination point */                        \
            break;                                                         \
        }                                                                  \
                                                                           \
        case DMR_FINALIZE:                                                 \
        {                                                                  \
            finalize_func;                                                 \
            dmr_finalize(); /* Termination point */                        \
            break;                                                         \
        }                                                                  \
                                                                           \
        case DMR_CLEANUP:                                                  \
        {                                                                  \
            finalize_func;                                                 \
            break;                                                         \
        }                                                                  \
                                                                           \
        default:                                                           \
        {                                                                  \
            printf("DMR_AUTO did not recognize the provided action.\n");   \
        }                                                                  \
        }                                                                  \
    }

#if defined(__cplusplus)
}
#endif

#endif // DMR_H