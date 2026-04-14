/**
 * @file dqr_core.c
 * @brief Core DQR context management and fragment bookkeeping.
 *
 * This module implements the in-memory DQR context lifecycle and basic
 * operations over the fragment list:
 * - Create/destroy and reset of the DQR context.
 * - Fragment insertion.
 * - Capacity getters/setters.
 * - Backlog accounting (pending/dispatched by label/route).
 * - Acknowledgement APIs to advance fragment state (DONE) or re-queue on failure.
 *
 * - All operations return 0 on invalid inputs.
 * - Fragment storage uses a growable array (realloc doubling strategy).
 * - The fragment ID is treated as a unique key within a context.
 */
#include "dqr.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "dqr_internal.h"

static int64_t g_dqr_core_t0_ms  = 0;
static int     g_dqr_core_t0_set = 0;
 
static double dqr_core_elapsed_s(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
    if (!g_dqr_core_t0_set) {
        g_dqr_core_t0_ms  = now_ms;
        g_dqr_core_t0_set = 1;
    }
    return (double)(now_ms - g_dqr_core_t0_ms) / 1000.0;
}
 
static int dqr_core_debug_level(void) {
    const char *v = getenv("DMR_QCUT_DQR_DEBUG");
    if (!v || !*v) return 0;
    return atoi(v);
}

/**
 * @brief Initialize a fragment slot to well-defined defaults.
 *
 * This helper zeroes the fragment memory and then applies explicit defaults
 * for label, route, state and failure bookkeeping fields.
 *
 * @param f Pointer to fragment slot to initialize (NOT NULL).
 */
static void dqr_zero_fragment(DQRFragment *f)
{
    memset(f, 0, sizeof(*f));
    f->label = DQR_LABEL_UNDECIDED;
    f->route = DQR_BACKEND_UNDEFINED;
    f->state = DQR_FRAG_PENDING;
    f->fail_count = 0;
    f->fail_permanent = 0;
}

/**
 * @brief Create a DQR context with optional initial configuration.
 *
 * Allocates a new context, initializes core fields, and allocates an initial
 * fragment array of fixed capacity (currently 32).
 *
 * If @p cfg is provided, selected fields are copied into the context:
 * - slurm_job_id (opaque identifier)
 * - capacity (resource availability hints)
 * - iter0_only (policy knob)
 *
 * @param cfg Optional configuration. May be NULL.
 *
 * @return A newly allocated context on success; NULL on allocation failure.
 */
DQRContext* dqr_create(const DQRConfig *cfg)
{
    DQRContext *ctx = (DQRContext*)calloc(1, sizeof(DQRContext));
    if(!ctx) return NULL;

    if(cfg)
    {
        ctx->slurm_job_id = cfg->slurm_job_id;
        ctx->cap = cfg->capacity;
        ctx->iter0_only = cfg->iter0_only;
    }

    ctx->iter = 0;
    ctx->cap_frags = 32;
    ctx->frags = (DQRFragment*)calloc((size_t)ctx->cap_frags, sizeof(DQRFragment));
    if(!ctx->frags)
    {
        free(ctx);
        return NULL;
    }
    return ctx;
}

/**
 * @brief Destroy a DQR context and release all owned memory.
 *
 * Safe to call with NULL.
 *
 * @param ctx Context to destroy (may be NULL).
 */
void dqr_destroy(DQRContext *ctx)
{
    if(!ctx) return;
    free(ctx->frags);
    free(ctx);
}

/**
 * @brief Reset the fragment list and iteration counter.
 *
 * This operation clears the logical fragment count and resets the iteration
 * to zero. The underlying fragment array allocation is retained for reuse.
 *
 * @param ctx Context to reset (NOT NULL).
 *
 * @return 1 on success; 0 if @p ctx is NULL.
 */
int dqr_reset_fragments(DQRContext *ctx)
{
    if(!ctx) return 0;
    ctx->n = 0;
    ctx->iter = 0;
    return 1;
}

/**
 * @brief Find the index of a fragment by fragment ID.
 *
 * Performs a linear search over the context fragment array. The fragment ID
 * is treated as the unique key for de-duplication and for ack operations.
 *
 * @param ctx     Context to search (NOT NULL).
 * @param frag_id Fragment identifier string (NOT NULL).
 *
 * @return Zero-based index if found; -1 if not found or on invalid inputs.
 */
static int dqr_find_frag_idx(const DQRContext *ctx, const char *frag_id)
{
    if(!ctx || !frag_id) return -1;
    for(int i=0;i<ctx->n;i++)
        if(strcmp(ctx->frags[i].frag_id, frag_id) == 0)
            return i;
    return -1;
}

/**
 * @brief Add a fragment to the context (defensive de-duplication).
 *
 * The fragment is copied by value into the context's internal fragment array.
 * If a fragment with the same @c frag_id already exists, the function performs
 * a defensive no-op and returns success.
 *
 * Storage strategy:
 * - If the array is full, capacity is doubled via realloc.
 *
 * Normalization:
 * - The destination slot is first initialized via @ref dqr_zero_fragment.
 * - Then a full struct copy is performed.
 * - If the resulting state is zero, it is normalized to @c DQR_FRAG_PENDING
 *   to avoid an "unset" state leaking into the state machine.
 *
 * @param ctx  Context (NOT NULL).
 * @param frag Fragment to add (NOT NULL). Its @c frag_id must be non-empty.
 *
 * @return 1 on success (including de-dup no-op); 0 on invalid inputs or allocation failure.
 */
int dqr_add_fragment(DQRContext *ctx, const DQRFragment *frag)
{
    if(!ctx || !frag || frag->frag_id[0] == '\0') return 0;

    if(dqr_find_frag_idx(ctx, frag->frag_id) >= 0)
        return 1;

    if(ctx->n >= ctx->cap_frags)
    {
        int new_cap = ctx->cap_frags * 2;
        DQRFragment *nf = (DQRFragment*)realloc(ctx->frags, (size_t)new_cap * sizeof(DQRFragment));
        if(!nf) return 0;
        ctx->frags = nf;
        ctx->cap_frags = new_cap;
    }

    DQRFragment *dst = &ctx->frags[ctx->n++];
    dqr_zero_fragment(dst);
    *dst = *frag;

    dst->state = (dst->state == 0) ? DQR_FRAG_PENDING : dst->state;
    return 1;
}

/**
 * @brief Set the current capacity snapshot for this DQR context.
 *
 * @param ctx Context (NOT NULL).
 * @param cap Capacity structure to copy into the context (NOT NULL).
 *
 * @return 1 on success; 0 on invalid inputs.
 */
int dqr_set_capacity(DQRContext *ctx, const DQRCapacity *cap)
{
    if(!ctx || !cap) return 0;
    ctx->cap = *cap;
    return 1;
}

/**
 * @brief Get the current capacity snapshot from this DQR context.
 *
 * @param ctx Context (NOT NULL).
 * @param out Output capacity structure (NOT NULL).
 *
 * @return 1 on success; 0 on invalid inputs.
 */
int dqr_get_capacity(const DQRContext *ctx, DQRCapacity *out)
{
    if(!ctx || !out) return 0;
    *out = ctx->cap;
    return 1;
}

/**
 * @brief Compute backlog and dispatch counters for all known fragments.
 *
 * This function aggregates the current fragment set into simple counters
 * useful for routing/planning policies:
 * - Total number of fragments
 * - Pending fragments by label (HPC/QC/UNDECIDED)
 * - Dispatched fragments by backend route (HPC/QC)
 *
 * The output structure is always zero-initialized before counting.
 *
 * @param ctx Context (NOT NULL).
 * @param out Output backlog counters (NOT NULL).
 *
 * @return 1 on success; 0 on invalid inputs.
 */
int dqr_get_backlog_counts(const DQRContext *ctx, DQRBacklogCounts *out)
{
    if(!ctx || !out) return 0;
    memset(out, 0, sizeof(*out));

    for(int i=0;i<ctx->n;i++)
    {
        const DQRFragment *f = &ctx->frags[i];
        out->total++;

        if(f->state == DQR_FRAG_PENDING)
        {
            if(f->label == DQR_LABEL_HPC) out->hpc_pending++;
            else if(f->label == DQR_LABEL_QC) out->qc_pending++;
            else out->undecided_pending++;
        }
        else if(f->state == DQR_FRAG_DISPATCHED)
        {
            if(f->route == DQR_BACKEND_HPC) out->hpc_dispatched++;
            else if(f->route == DQR_BACKEND_QC) out->qc_dispatched++;
        }
    }
    return 1;
}

/**
 * @brief Acknowledge a fragment as completed (DONE).
 *
 * Marks the target fragment as @c DQR_FRAG_DONE if it exists.
 *
 * @param ctx     Context (NOT NULL).
 * @param frag_id Fragment identifier (NOT NULL).
 *
 * @return 1 if the fragment was found and updated; 0 otherwise.
 */
int dqr_ack_fragment_done(DQRContext *ctx, const char *frag_id)
{
    if(!ctx || !frag_id) return 0;
    int idx = dqr_find_frag_idx(ctx, frag_id);
    if(idx < 0) return 0;
    ctx->frags[idx].state = DQR_FRAG_DONE;
    if (dqr_core_debug_level() >= 1) {
        fprintf(stderr, "[RANK0] T=%.3f DONE frag_id=%s\n",
                dqr_core_elapsed_s(), frag_id);
        fflush(stderr);
    }
    return 1;
}

static int env_int(const char *k, int defv){
    const char *v = getenv(k);
    if(!v || !*v) return defv;
    return atoi(v);
}


/**
 * @brief Acknowledge a fragment execution failure and re-queue it.
 *
 * This API transitions the fragment back to a re-dispatchable state:
 * - Sets @c state to @c DQR_FRAG_PENDING.
 * - Resets @c route to @c DQR_BACKEND_UNDEFINED.
 *
 * It also updates failure bookkeeping:
 * - Increments @c fail_count.
 * - Sets @c fail_permanent depending on @p kind.
 *
 * The failure reason string is cleared. The planner is expected to populate
 * a new reason on the next plan/commit step.
 *
 * @param ctx     Context (NOT NULL).
 * @param frag_id Fragment identifier (NOT NULL).
 * @param kind    Failure kind classification. If @c DQR_FAIL_PERMANENT, the
 *                fragment is marked as permanently failed in bookkeeping.
 *
 * @return 1 if the fragment was found and updated; 0 otherwise.
 */
int dqr_ack_fragment_failed(DQRContext *ctx, const char *frag_id, DQRAckFailureKind kind)
{
    if(!ctx || !frag_id) return 0;
    int idx = dqr_find_frag_idx(ctx, frag_id);
    if(idx < 0) return 0;

    DQRFragment *f = &ctx->frags[idx];

    f->fail_count += 1;

    int maxr = env_int("DQR_MAX_TRANSIENT_RETRIES", 3);
    if(kind == DQR_FAIL_TRANSIENT && f->fail_count >= maxr){
        kind = DQR_FAIL_PERMANENT;
    }

    if(kind == DQR_FAIL_PERMANENT){

        f->fail_permanent = 1;

        /* Failover QC→HPC: re-label and keep routable */
        const char *fov = getenv("DQR_ALLOW_FAILOVER_QC_TO_HPC");
        if(fov && strcmp(fov, "1") == 0 && f->label == DQR_LABEL_QC){
            f->label = DQR_LABEL_HPC;
            f->state = DQR_FRAG_PENDING;
            f->route = DQR_BACKEND_UNDEFINED;
            return 2;  // 2 = permanent, but re-labelled for failover
        }

        f->state = DQR_FRAG_FAILED;     // terminal (no failover)
        f->route = DQR_BACKEND_UNDEFINED;
        return 2;
    }
 
    // transient: requeue
    f->fail_permanent = 0;
    f->state = DQR_FRAG_PENDING;
    f->route = DQR_BACKEND_UNDEFINED;
 
    return 1;  // 1 = re-queued (transient)
}

/**
 * @brief Get the number of fragments currently registered in the context.
 *
 * @param ctx Context (may be NULL).
 *
 * @return Fragment count as size_t; 0 if @p ctx is NULL.
 */
size_t dqr_get_fragment_count(const DQRContext *ctx)
{
    if(!ctx) return 0;
    return (size_t)ctx->n;
}

/**
 * @brief Get a pointer to a fragment by index.
 *
 * The returned pointer is owned by the context and remains valid until the
 * context is destroyed or the fragment storage is reallocated (e.g., by
 * adding fragments beyond current capacity).
 *
 * @param ctx Context (NOT NULL).
 * @param idx Zero-based fragment index.
 *
 * @return Pointer to fragment on success; NULL if out-of-range or on NULL ctx.
 */
const DQRFragment* dqr_get_fragment(const DQRContext *ctx, size_t idx)
{
    if(!ctx) return NULL;
    if(idx >= (size_t)ctx->n) return NULL;
    return &ctx->frags[(int)idx];
}

/**
 * @brief Get the current iteration counter from the DQR context.
 *
 * @param ctx Context (NOT NULL).
 *
 * @return Current iteration value; -1 if @p ctx is NULL.
 */
int dqr_get_iter(const DQRContext *ctx)
{
    if(!ctx) return -1;
    return ctx->iter;
}
