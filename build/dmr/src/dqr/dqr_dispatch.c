/**
 * @file dqr_dispatch.c
 * @brief Dispatch planning and commit utilities for DQR.
 *
 * This module builds a dispatch plan from the current context (via router
 * selection), computes a stable plan identifier, and applies ("commits") the
 * plan by transitioning selected fragments from PENDING to DISPATCHED.
 *
 * Key properties:
 * - Planning allocates index arrays sized to the worst-case fragment count.
 * - Commit is designed to be idempotent w.r.t. the context iteration; a plan
 *   whose iter does not match the current ctx->iter is treated as a no-op.
 * - Each commit closes a "wave/iteration" and increments ctx->iter, even for
 *   empty plans.
 *
 * The router selection logic is delegated to an internal router function
 * declared in this file.
 */
#include "dqr_internal.h"
#include <stdlib.h>
#include <string.h>

#include "dqr_slurm.h"
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief One step of the 64-bit FNV-1a hash.
 *
 * This helper mixes a 64-bit word into the running hash value using the
 * FNV-1a algorithm constants.
 *
 * @param h Current hash state.
 * @param x Word to mix into the hash.
 *
 * @return Updated hash state after mixing @p x.
 */
static uint64_t fnv1a64_step(uint64_t h, uint64_t x)
{
    const uint64_t FNV_PRIME = 1099511628211ULL;
    h ^= x;
    h *= FNV_PRIME;
    return h;
}

/**
 * @brief Compute a stable identifier for a dispatch plan.
 *
 * The identifier is derived from:
 * - plan iteration
 * - HPC/QC selection counts
 * - selected fragment indices for each backend group
 *
 * This provides a lightweight fingerprint for plan equivalence and debugging.
 * A computed hash of zero is remapped to 1 to avoid ambiguous "unset" IDs.
 *
 * @param p Dispatch plan to hash (may be NULL).
 *
 * @return Non-zero plan ID on success; 0 if @p p is NULL.
 */
static uint64_t compute_plan_id(const DQRDispatchPlan *p)
{
    if(!p) return 0;
    uint64_t h = 1469598103934665603ULL;
    h = fnv1a64_step(h, (uint64_t)(uint32_t)p->iter);
    h = fnv1a64_step(h, (uint64_t)(uint32_t)p->hpc_count);
    h = fnv1a64_step(h, (uint64_t)(uint32_t)p->qc_count);
    for(int i=0;i<p->hpc_count;i++)
        h = fnv1a64_step(h, (uint64_t)(uint32_t)p->hpc_indices[i]);
    for(int i=0;i<p->qc_count;i++)
        h = fnv1a64_step(h, (uint64_t)(uint32_t)p->qc_indices[i]);
    if(h == 0) h = 1;
    return h;
}

/**
 * @brief Select fragments for the next dispatch wave (internal router API).
 *
 * This function is implemented in the router layer and is responsible for
 * selecting indices of fragments eligible to be dispatched to HPC and QC
 * backends, subject to policy/capacity constraints.
 *
 * The caller provides output buffers for indices and receives the counts.
 *
 * @param ctx         DQR context containing fragments and current state.
 * @param out_hpc_idx Output array for selected HPC fragment indices.
 * @param out_hpc_n   Output count of selected HPC indices.
 * @param out_qc_idx  Output array for selected QC fragment indices.
 * @param out_qc_n    Output count of selected QC indices.
 * @param max_out     Maximum number of indices that can be written to each output array.
 *
 * @return 1 on success; 0 on failure.
 */
int dqr_router_select(DQRContext *ctx, int *out_hpc_idx, int *out_hpc_n,
                      int *out_qc_idx, int *out_qc_n, int max_out);

static int get_int_env_default_local(const char *k, int defv)
{
    const char *v = getenv(k);
    if(!v || !v[0]) return defv;
    return atoi(v);
}

static void dqr_autosync_capacity_from_slurm(DQRContext *ctx)
{
    if(!ctx) return;

    DQRSlurmAlloc a;
    int ok = dqr_slurm_query_allocation(ctx->slurm_job_id, &a);

    if(ok && a.ntasks_alloc > 0)
    {
        int slots = a.ntasks_alloc - 1;          // exclude rank0
        if (slots < 0) slots = 0;
        ctx->cap.hpc_slots_total = slots;
    }
    else
    {
        //
    }

    int qc_slots = get_int_env_default_local("DQR_QC_SLOTS_TOTAL",
                    get_int_env_default_local("DMR_QCUT_QC_SLOTS_TOTAL", 0));
    if(qc_slots < 0) qc_slots = 0;
    ctx->cap.qc_slots_total = qc_slots;

    int qc_deg = get_int_env_default_local("DQR_QC_DEGRADED",
                 get_int_env_default_local("DMR_QCUT_QC_DEGRADED", 0));
    ctx->cap.qc_degraded = (qc_deg != 0);

    int hpc_deg = get_int_env_default_local("DQR_HPC_DEGRADED", 0);
    ctx->cap.hpc_degraded = (hpc_deg != 0);
}

/**
 * @brief Build the next dispatch plan from the current context.
 *
 * Initializes @p out_plan, sets its iteration to @c ctx->iter, allocates
 * worst-case index arrays (size = number of fragments), and delegates the
 * selection logic to the internal router. On success, also computes a stable
 * @c plan_id derived from the plan content.
 *
 * Memory ownership:
 * - On success, @p out_plan owns the allocated index arrays and must be
 *   released with @ref dqr_free_dispatch_plan.
 * - On any failure, this function releases any partially allocated memory
 *   and returns 0, leaving @p out_plan zeroed.
 *
 * @param ctx      DQR context (NOT NULL).
 * @param out_plan Output plan structure to initialize and fill (NOT NULL).
 *
 * @return 1 on success; 0 on invalid inputs, allocation failure, or router failure.
 */
int dqr_plan_next_dispatch(DQRContext *ctx, DQRDispatchPlan *out_plan)
{
    if(!ctx || !out_plan) return 0;
    dqr_autosync_capacity_from_slurm(ctx);
    memset(out_plan, 0, sizeof(*out_plan));
    out_plan->iter = ctx->iter;

    out_plan->hpc_indices = (int*)calloc((size_t)ctx->n, sizeof(int));
    out_plan->qc_indices  = (int*)calloc((size_t)ctx->n, sizeof(int));
    if(!out_plan->hpc_indices || !out_plan->qc_indices)
    {
        dqr_free_dispatch_plan(out_plan);
        return 0;
    }

    int ok = dqr_router_select(ctx,
                              out_plan->hpc_indices, &out_plan->hpc_count,
                              out_plan->qc_indices,  &out_plan->qc_count,
                              ctx->n);
    if(!ok)
    {
        dqr_free_dispatch_plan(out_plan);
        return 0;
    }
    out_plan->plan_id = compute_plan_id(out_plan);
    return 1;
}

/**
 * @brief Free resources owned by a dispatch plan and reset it to zero.
 *
 * This function is safe to call with NULL, and safe to call multiple times
 * on the same plan (idempotent cleanup).
 *
 * @param plan Plan to free (may be NULL).
 */
void dqr_free_dispatch_plan(DQRDispatchPlan *plan)
{
    if(!plan) return;
    free(plan->hpc_indices);
    free(plan->qc_indices);
    memset(plan, 0, sizeof(*plan));
}

/**
 * @brief Commit a dispatch plan into the context state machine.
 *
 * A commit applies the plan by transitioning selected fragments from
 * @c DQR_FRAG_PENDING to @c DQR_FRAG_DISPATCHED. Indices that are out of range
 * are ignored.
 *
 * Idempotency policy:
 * - If @c plan->iter does not match @c ctx->iter, the function returns success
 *   without applying any changes (no-op), preventing re-commit across waves.
 *
 * Wave closure:
 * - Regardless of whether any fragments were dispatched (including empty plans),
 *   the function increments @c ctx->iter to close the current wave.
 *
 * @param ctx  DQR context (NOT NULL).
 * @param plan Dispatch plan to commit (NOT NULL).
 *
 * @return 1 on success (including idempotent no-op); 0 on invalid inputs.
 */
int dqr_commit_dispatch(DQRContext *ctx, const DQRDispatchPlan *plan)
{
    if(!ctx || !plan) return 0;

    if(plan->iter != ctx->iter)
        return 1;

    for(int i=0;i<plan->hpc_count;i++)
    {
        int idx = plan->hpc_indices[i];
        if(idx >= 0 && idx < ctx->n)
        {
            if(ctx->frags[idx].state == DQR_FRAG_PENDING)
                ctx->frags[idx].state = DQR_FRAG_DISPATCHED;
        }
    }

    for(int i=0;i<plan->qc_count;i++)
    {
        int idx = plan->qc_indices[i];
        if(idx >= 0 && idx < ctx->n)
        {
            if(ctx->frags[idx].state == DQR_FRAG_PENDING)
                ctx->frags[idx].state = DQR_FRAG_DISPATCHED;
        }
    }

    ctx->iter++;

    return 1;
}
