/**
 * @file dqr_internal.h
 * @brief Internal DQR declarations.
 */
#ifndef DQR_INTERNAL_H
#define DQR_INTERNAL_H

#include "dqr.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct DQRContext
 * @brief Concrete internal representation of a DQR context.
 *
 * The context tracks:
 * - A job identifier (e.g., Slurm job id) for correlation and diagnostics.
 * - A capacity snapshot used by routing and dispatch planning.
 * - The current wave iteration counter @c iter.
 * - A policy knob to restrict routing decisions in iter >= 1 @c iter0_only.
 * - A growable array of fragments and its current size/capacity.
 *
 * Ownership:
 * - The context owns the @c frags array and is responsible for allocating
 *   and freeing it (see dqr_create/dqr_destroy in the core implementation).
 */
struct DQRContext {
    uint32_t    slurm_job_id;   // Slurm job id associated with this context.
    DQRCapacity cap;            //Current dispatch "wave" iteration.
    int         iter;           //Current dispatch "wave" iteration. 
    int         iter0_only;     //When non-zero, iteration >= 1 may block UNDECIDED routing decisions

    DQRFragment *frags;         //Owned fragment array
    int          n;             //Number of valid fragments currently stored in @c frags.
    int          cap_frags;     //Allocated capacity (in elements) of @c frags.
};

/**
 * @brief Internal router primitive used by the dispatch planner.
 *
 * Selects fragment indices to be dispatched in the current wave into two
 * output arrays (HPC/QC). In addition to producing index lists, the router
 * may update per-fragment routing fields (e.g., @c route and @c reason) to
 * make decisions traceable and deterministic.
 *
 * Output semantics:
 * - The router writes selected indices to @p out_hpc_idx and @p out_qc_idx.
 * - The router writes counts to @p out_hpc_n and @p out_qc_n.
 *
 * Capacity and truncation:
 * - @p max_out is a caller-provided bound for how many indices can be written
 *   to each output array. The current implementation may ignore this bound
 *   for test determinism (see router implementation).
 *
 * @param ctx         DQR context (must not be NULL).
 * @param out_hpc_idx Output array for selected HPC indices (must not be NULL).
 * @param out_hpc_n   Output count for HPC selections (must not be NULL).
 * @param out_qc_idx  Output array for selected QC indices (must not be NULL).
 * @param out_qc_n    Output count for QC selections (must not be NULL).
 * @param max_out     Maximum number of indices writable to each output array.
 *
 * @return 1 on success; 0 on failure/invalid inputs.
 */
int dqr_router_select(
    struct DQRContext *ctx,
    int *out_hpc_idx, int *out_hpc_n,
    int *out_qc_idx,  int *out_qc_n,
    int max_out
);

#ifdef __cplusplus
}
#endif

#endif // DQR_INTERNAL_H
