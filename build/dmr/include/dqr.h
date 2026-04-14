/**
 * @file dqr.h
 * @brief Public API for the DQR (Dispatch & QoS Router) component.
 *
 * DQR provides in-memory bookkeeping and routing/dispatch planning for a set of
 * fragments (e.g., produced by a circuit cutting pipeline). It supports:
 * - Ingestion of fragments with labels (HPC/QC/UNDECIDED)
 * - Capacity-aware routing decisions to HPC/QC backends
 * - Planning and committing dispatch "waves" (iterations)
 * - Acknowledging completion and failures for feedback into subsequent routing
 * - Read-only introspection and optional trace export
 */
#ifndef DQR_H
#define DQR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup dqr_api DQR Public API
 * @brief Public types and functions for DQR.
 *
 * The public API keeps @c DQRContext opaque. Users interact via lifecycle, 
 * ingestion, planning, acknowledgements, and read-only introspection functions.
 * @{
 */

/**
 * @enum DQRBackend
 * @brief Backend routing target selected by DQR.
 *
 * @details
 * - @c DQR_BACKEND_HPC: Fragment should be dispatched to the classical HPC backend.
 * - @c DQR_BACKEND_QC: Fragment should be dispatched to the quantum backend (QPU).
 * - @c DQR_BACKEND_UNDEFINED: No dispatch decision / not dispatchable in current wave.
 */
typedef enum {
    DQR_BACKEND_HPC = 0,
    DQR_BACKEND_QC  = 1,
    DQR_BACKEND_UNDEFINED = 2
} DQRBackend;

/**
 * @enum DQRLabel
 * @brief Input label for a fragment (upstream classification/hint).
 *
 * @details
 * Labels are typically produced by an upstream pipeline (partitioning, estimation,
 * heuristics) and provide a hint to the router. DQR may route a fragment
 * differently depending on policy/capacity.
 *
 * - @c DQR_LABEL_HPC: Prefer classical HPC execution.
 * - @c DQR_LABEL_QC: Prefer quantum backend execution.
 * - @c DQR_LABEL_UNDECIDED: No preference; routed according to policy and capacity.
 */
typedef enum {
    DQR_LABEL_HPC = 0,
    DQR_LABEL_QC  = 1,
    DQR_LABEL_UNDECIDED = 2
} DQRLabel;

/**
 * @enum DQRFragState
 * @brief Fragment state within DQR's internal state machine.
 *
 * @details
 * - @c DQR_FRAG_PENDING: Eligible for routing/dispatch planning.
 * - @c DQR_FRAG_DISPATCHED: Selected and committed for execution in current/previous wave.
 * - @c DQR_FRAG_DONE: Execution completed successfully.
 * - @c DQR_FRAG_FAILED: Failure state (note: current implementation typically re-queues
 *   failed fragments back to PENDING rather than leaving them in FAILED).
 */
typedef enum {
    DQR_FRAG_PENDING = 0,
    DQR_FRAG_DISPATCHED = 1,
    DQR_FRAG_DONE = 2,
    DQR_FRAG_FAILED = 3
} DQRFragState;

/**
 * @struct DQRBacklogCounts
 * @brief Aggregated backlog statistics for fragments in a context.
 *
 * @details
 * Counters are split by:
 * - Pending fragments: by label (HPC/QC/UNDECIDED)
 * - Dispatched fragments: by backend route (HPC/QC)
 * - Total fragments tracked
 */
typedef struct {
    int hpc_pending;        /**< Number of PENDING fragments labelled HPC. */
    int qc_pending;         /**< Number of PENDING fragments labelled QC. */
    int undecided_pending;  /**< Number of PENDING fragments labelled UNDECIDED. */

    int hpc_dispatched;     /**< Number of DISPATCHED fragments routed to HPC. */
    int qc_dispatched;      /**< Number of DISPATCHED fragments routed to QC. */

    int total;              /**< Total fragments tracked in the context. */
} DQRBacklogCounts;

/**
 * @struct DQRCapacity
 * @brief Capacity snapshot used by routing/dispatch policy.
 *
 * @details
 * Capacity represents the available execution slots for the current wave.
 * Implementations may subtract "in-flight" fragments from totals to compute
 * remaining availability.
 *
 * Degradation flags provide backpressure signals (e.g., QPU down, saturated queues)
 * and typically cause routing to avoid the degraded backend.
 */
typedef struct {
    int hpc_slots_total;  /**< Total available HPC slots (e.g., MPI tasks) for this wave. */
    int qc_slots_total;   /**< Total available QC slots (e.g., QPU sessions) for this wave. */

    int qc_degraded;      /**< QC degraded flag (0/1). When set, QC is treated as unavailable. */
    int hpc_degraded;     /**< HPC degraded flag (0/1). When set, HPC is treated as unavailable. */
} DQRCapacity;

/**
 * @struct DQRFragment
 * @brief Unit of scheduling/routing managed by DQR.
 *
 * @details
 * A fragment is identified by a stable string ID (e.g., "frag_000") and carries:
 * - An upstream label (HPC/QC/UNDECIDED)
 * - A router decision (@c route) and current state (@c state)
 * - Optional circuit metadata (qubits/depth/two-qubit gates)
 * - A deterministic routing reason string for traceability/tests
 * - Failure bookkeeping for policy decisions on subsequent waves
 *
 * @note
 * The meaning of @c reason is policy- and iteration-dependent; it is intended
 * for debugging and deterministic tests rather than end-user messaging.
 */
typedef struct {
    char frag_id[64];        /**< Canonical fragment identifier (must be non-empty). */
    DQRLabel label;          /**< Upstream label: HPC/QC/UNDECIDED. */
    DQRBackend route;        /**< Router decision: HPC/QC/UNDEFINED. */
    DQRFragState state;      /**< Fragment state: PENDING/DISPATCHED/DONE/FAILED. */

    int num_qubits;          /**< Optional: estimated qubit count. */
    int depth;               /**< Optional: estimated circuit depth. */
    int two_qubit_gates;     /**< Optional: estimated two-qubit gate count. */

    char reason[96];         /**< Debug reason code for the current routing decision. */

    int fail_count;          /**< Number of acknowledged failures for this fragment. */
    int fail_permanent;      /**< 1 if last acknowledged failure was permanent; 0 otherwise. */
} DQRFragment;

/**
 * @typedef DQRContext
 * @brief Opaque context handle for DQR.
 *
 * @details
 * The concrete structure is private to the DQR implementation. Users must create
 * and destroy contexts using @ref dqr_create and @ref dqr_destroy.
 */
typedef struct DQRContext DQRContext;

/**
 * @struct DQRConfig
 * @brief Configuration structure for creating a DQR context.
 *
 * @details
 * - @c slurm_job_id: Used to correlate DQR decisions with a scheduler job.
 * - @c capacity: Initial capacity snapshot. Can be updated later via @ref dqr_set_capacity.
 * - @c iter0_only: Debug/policy knob. When enabled, iter>=1 may block re-routing
 *   of UNDECIDED fragments depending on router implementation.
 */
typedef struct {
    uint32_t slurm_job_id;   /**< Job identity (for integration/tracing). */
    DQRCapacity capacity;    /**< Initial capacity snapshot (may be updated later). */
    int iter0_only;          /**< If 1: restrict routing beyond iter 0 (debug/policy knob). */
} DQRConfig;

/** @name Lifecycle
 *  @{
 */
/**
 * @brief Create a new DQR context.
 *
 * Allocates and initializes a context. If @p cfg is NULL, implementation defaults
 * are used.
 *
 * @param cfg Optional configuration (may be NULL).
 * @return Pointer to a new context; NULL on allocation failure.
 */
DQRContext* dqr_create(const DQRConfig *cfg);

/**
 * @brief Destroy a DQR context and release owned memory.
 *
 * Safe to call with NULL.
 *
 * @param ctx Context to destroy (may be NULL).
 */
void dqr_destroy(DQRContext *ctx);

/** @} */

/** @name Fragment ingestion
 *  @{
 */

/**
 * @brief Clear the fragment set and reset the iteration counter.
 *
 * The underlying storage may be retained for reuse.
 *
 * @param ctx Context (must not be NULL).
 * @return 1 on success; 0 on invalid inputs.
 */
int dqr_reset_fragments(DQRContext *ctx);

/**
 * @brief Add a fragment to the context.
 *
 * The fragment is copied into the context. Implementations may defensively
 * de-duplicate by @c frag_id.
 *
 * @param ctx  Context (must not be NULL).
 * @param frag Fragment to add (must not be NULL). @c frag->frag_id must be non-empty.
 *
 * @return 1 on success; 0 on invalid inputs or allocation failure.
 */
int dqr_add_fragment(DQRContext *ctx, const DQRFragment *frag);

/** @} */

/** @name Capacity management
 *  @{
 */

/**
 * @brief Update the capacity snapshot used by routing and dispatch planning.
 *
 * @param ctx Context (must not be NULL).
 * @param cap Capacity snapshot to set (must not be NULL).
 *
 * @return 1 on success; 0 on invalid inputs.
 */
int dqr_set_capacity(DQRContext *ctx, const DQRCapacity *cap);

/**
 * @brief Retrieve the current capacity snapshot.
 *
 * @param ctx Context (must not be NULL).
 * @param out Output capacity snapshot (must not be NULL).
 *
 * @return 1 on success; 0 on invalid inputs.
 */
int dqr_get_capacity(const DQRContext *ctx, DQRCapacity *out);

/** @} */

/** @name Backlog queries
 *  @{
 */

/**
 * @brief Compute backlog counters for the current fragment set.
 *
 * @param ctx Context (must not be NULL).
 * @param out Output counters (must not be NULL).
 *
 * @return 1 on success; 0 on invalid inputs.
 */
int dqr_get_backlog_counts(const DQRContext *ctx, DQRBacklogCounts *out);

/** @} */

/** @name Dispatch planning
 *  @{
 */

/**
 * @struct DQRDispatchPlan
 * @brief Planned dispatch wave (indices to dispatch to HPC/QC).
 *
 * @details
 * A plan is produced by @ref dqr_plan_next_dispatch and must be committed using
 * @ref dqr_commit_dispatch. The plan owns dynamically allocated index arrays.
 *
 * Ownership:
 * - On success, @c hpc_indices and @c qc_indices are owned by the plan and must be
 *   released with @ref dqr_free_dispatch_plan.
 *
 * Idempotency:
 * - @c plan_id provides a stable identifier derived from the plan content.
 * - Implementations may treat a plan whose @c iter differs from the current context
 *   iteration as a no-op on commit.
 */
typedef struct {
    int iter;                 /**< DQR iteration number this plan applies to. */
    uint64_t plan_id;         /**< Stable identifier for the planned wave. */
    int hpc_count;            /**< Number of selected HPC indices. */
    int qc_count;             /**< Number of selected QC indices. */
    int *hpc_indices;         /**< Indices into the internal fragment array (HPC selections). */
    int *qc_indices;          /**< Indices into the internal fragment array (QC selections). */
} DQRDispatchPlan;

/**
 * @brief Plan the next dispatch wave from the current context.
 *
 * Allocates index arrays inside @p out_plan. On success, the caller must free
 * the plan using @ref dqr_free_dispatch_plan.
 *
 * @param ctx      Context (must not be NULL).
 * @param out_plan Output plan to fill (must not be NULL).
 *
 * @return 1 on success; 0 on invalid inputs, allocation failure, or routing failure.
 */
int dqr_plan_next_dispatch(DQRContext *ctx, DQRDispatchPlan *out_plan);

/**
 * @brief Free resources owned by a dispatch plan and reset it to zero.
 *
 * Safe to call with NULL.
 *
 * @param plan Plan to free (may be NULL).
 */
void dqr_free_dispatch_plan(DQRDispatchPlan *plan);

/**
 * @brief Commit a dispatch plan into the context.
 *
 * Typically transitions selected fragments from PENDING to DISPATCHED and closes
 * the current wave. Implementations may treat commits with mismatched iteration
 * as idempotent no-ops.
 *
 * @param ctx  Context (must not be NULL).
 * @param plan Plan to commit (must not be NULL).
 *
 * @return 1 on success; 0 on invalid inputs.
 */
int dqr_commit_dispatch(DQRContext *ctx, const DQRDispatchPlan *plan);

/** @} */

/** @name Acknowledgements
 *  @{
 */

/**
 * @enum DQRAckFailureKind
 * @brief Classification of fragment failures for policy decisions.
 *
 * @details
 * - @c DQR_FAIL_TRANSIENT: Eligible for retry on the same backend if policy allows.
 * - @c DQR_FAIL_PERMANENT: Eligible for failover (e.g., QC->HPC) if policy allows.
 */
typedef enum DQRAckFailureKind {
    DQR_FAIL_TRANSIENT = 0,
    DQR_FAIL_PERMANENT = 1
} DQRAckFailureKind;

/**
 * @brief Acknowledge a fragment failure.
 *
 * Failure classification influences subsequent routing decisions:
 * - TRANSIENT: eligible for retry on same backend if policy allows.
 * - PERMANENT: eligible for failover (e.g., QC->HPC) if policy allows.
 *
 * Typical side effects include re-queuing the fragment (PENDING), resetting route,
 * updating failure counters, and clearing the current reason string.
 *
 * @param ctx     Context (must not be NULL).
 * @param frag_id Fragment identifier (must not be NULL).
 * @param kind    Failure classification.
 *
 * @return 1 if the fragment was found and updated; 0 otherwise.
 */
int dqr_ack_fragment_failed(DQRContext *ctx, const char *frag_id, DQRAckFailureKind kind);

/**
 * @brief Acknowledge a fragment completion (DONE).
 *
 * @param ctx     Context (must not be NULL).
 * @param frag_id Fragment identifier (must not be NULL).
 *
 * @return 1 if the fragment was found and updated; 0 otherwise.
 */
int dqr_ack_fragment_done(DQRContext *ctx, const char *frag_id);

/** @} */

/** @name Optional trace export
 *  @{
 */

/**
 * @brief Write a CSV trace snapshot of the context to a job directory.
 *
 * The trace is intended for debugging and post-mortem analysis; it is not required
 * by the routing logic.
 *
 * @param ctx     Context (must not be NULL).
 * @param job_dir Output directory path (must not be NULL or empty).
 * @param iter    Iteration identifier for the trace. Implementations may use
 *                @c ctx->iter when @p iter is negative for the CSV content.
 *
 * @return 1 on success; 0 on invalid inputs or I/O failure.
 */
int dqr_write_trace_csv(const DQRContext *ctx, const char *job_dir, int iter);

/** @} */

/** @name Read-only introspection
 *  @{
 */

/**
 * @brief Get the number of fragments in the context.
 *
 * @param ctx Context (may be NULL).
 * @return Fragment count; 0 if @p ctx is NULL.
 */
size_t dqr_get_fragment_count(const DQRContext *ctx);

/**
 * @brief Get a pointer to a fragment by index (read-only).
 *
 * Lifetime rule:
 * The returned pointer is owned by DQR and remains valid until the next call
 * that may mutate the internal fragment array (e.g., reset/add/commit/ack).
 *
 * @param ctx Context (must not be NULL).
 * @param idx Zero-based index.
 *
 * @return Pointer to fragment on success; NULL on invalid inputs or out-of-range.
 */
const DQRFragment* dqr_get_fragment(const DQRContext *ctx, size_t idx);

/**
 * @brief Get the current DQR iteration counter.
 *
 * @param ctx Context (must not be NULL).
 * @return Iteration value; -1 if @p ctx is NULL.
 */
int dqr_get_iter(const DQRContext *ctx);

/** @} */

/** @} */ // end of dqr_api

#ifdef __cplusplus
}
#endif

#endif // DQR_H
