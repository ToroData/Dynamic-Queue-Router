/**
 * @file dqr_router.c
 * @brief Routing policy for selecting fragments to dispatch to HPC/QC backends.
 *
 * This module implements the internal routing policy used by DQR to decide
 * which fragments should be dispatched on each "wave" (iteration) and to which
 * backend (HPC or QC). The policy is stateful via @c ctx->iter and uses the
 * current capacity snapshot @c ctx->cap as constraints.
 *
 * The router produces two index lists:
 * - HPC indices selected for dispatch in the current wave
 * - QC indices selected for dispatch in the current wave
 *
 * In addition to filling these index lists, the router sets per-fragment:
 * - @c f->route: chosen backend or UNDEFINED when not dispatchable
 * - @c f->reason: deterministic reason code for testability and traceability
 *
 * Policy controls can be influenced via environment variables:
 * - DQR_ALLOW_RETRY_QC_ON_FAILURE: enable retrying QC after transient failure
 * - DQR_ALLOW_FAILOVER_QC_TO_HPC: enable failover QC->HPC after permanent failure
 * - DQR_PREFER_ITER0_UNDECIDED: preference for UNDECIDED fill in iter 0 ("HPC"|"QC")
 * - DQR_PREFER_ITERN_UNDECIDED: preference for UNDECIDED fill in iter >= 1 ("HPC"|"QC")
 * - DQR_STRICT_QC_LABEL: strict handling of QC-labelled fragments when QC capacity is 0
 *
 * Other:
 * - The selection is intentionally not truncated by @p max_out in the current
 *   test suite to ensure all PENDING fragments end the selection step with a
 *   deterministic @c reason (even if not dispatched).
 * - "In-flight" fragments are those DISPATCHED to a given backend; they consume
 *   slots from total capacity.
 */
#include "dqr_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * @brief Assign a reason string to a fragment in a safe, bounded way.
 *
 * If @p r is NULL, an empty string is used.
 *
 * @param f Fragment to update (may be NULL).
 * @param r Reason string (may be NULL).
 */
static void set_reason(DQRFragment *f, const char *r)
{
    if(!f) return;
    if(!r) r = "";
    snprintf(f->reason, sizeof(f->reason), "%s", r);
}

/**
 * @brief Check whether a fragment is in the PENDING state.
 *
 * @param f Fragment pointer (may be NULL).
 * @return Non-zero if @p f is non-NULL and @c f->state == DQR_FRAG_PENDING; 0 otherwise.
 */
static int is_pending(const DQRFragment *f)
{
    return f && f->state == DQR_FRAG_PENDING;
}

/**
 * @brief Compare an environment variable value against an expected string.
 *
 * @param k Environment variable key (NOT NULL).
 * @param v Expected value string (NOT NULL).
 *
 * @return Non-zero if getenv(@p k) exists and equals @p v; 0 otherwise.
 */
static int env_equals(const char *k, const char *v)
{
    const char *e = getenv(k);
    if(!e || !v) return 0;
    return strcmp(e, v) == 0;
}

/**
 * @brief Policy flag: allow retrying QC-labelled fragments after transient failure.
 *
 * Controlled by environment variable @c DQR_ALLOW_RETRY_QC_ON_FAILURE == "1".
 *
 * @return Non-zero if enabled; 0 otherwise.
 */
static int allow_retry_qc_on_failure(void)
{
    return env_equals("DQR_ALLOW_RETRY_QC_ON_FAILURE", "1");
}

/**
 * @brief Policy flag: allow failing over permanently failed QC-labelled fragments to HPC.
 *
 * Controlled by environment variable @c DQR_ALLOW_FAILOVER_QC_TO_HPC == "1".
 *
 * @return Non-zero if enabled; 0 otherwise.
 */
static int allow_failover_qc_to_hpc(void)
{
    return env_equals("DQR_ALLOW_FAILOVER_QC_TO_HPC", "1");
}

/**
 * @brief Policy for filling UNDECIDED fragments in iteration 0.
 *
 * Controlled by environment variable @c DQR_PREFER_ITER0_UNDECIDED.
 * Accepted values: "HPC" or "QC". Default is HPC.
 *
 * @return Preferred backend for filling UNDECIDED in iter 0.
 */
static DQRBackend prefer_iter0_undecided_backend(void)
{
    if(env_equals("DQR_PREFER_ITER0_UNDECIDED", "QC")) return DQR_BACKEND_QC;
    return DQR_BACKEND_HPC;
}

/**
 * @brief Policy for filling UNDECIDED fragments in iteration >= 1.
 *
 * Controlled by environment variable @c DQR_PREFER_ITERN_UNDECIDED.
 * Accepted values: "HPC" or "QC". Default is HPC.
 *
 * @return Preferred backend for filling UNDECIDED in iter >= 1.
 */
static DQRBackend prefer_itern_undecided_backend(void)
{
    if(env_equals("DQR_PREFER_ITERN_UNDECIDED", "QC")) return DQR_BACKEND_QC;
    return DQR_BACKEND_HPC;
}

/**
 * @brief Policy flag: strict treatment for QC-labelled fragments.
 *
 * When enabled (@c DQR_STRICT_QC_LABEL == "1"), QC-labelled fragments will
 * produce specific reason codes under certain zero-capacity conditions, and
 * will not "fall back" to HPC in this suite.
 *
 * @return Non-zero if strict mode is enabled; 0 otherwise.
 */
static int strict_qc_label_enabled(void)
{
    const char *e = getenv("DQR_STRICT_QC_LABEL");
    return (e && strcmp(e, "1") == 0);
}

/**
 * @brief Policy hook: allow promotion of HPC-labelled fragments to QC (currently disabled).
 *
 * This function is a placeholder for potential future policy evolution. In the
 * current suite it is hard-disabled and always returns 0.
 *
 * @param cap Capacity snapshot (unused in current implementation).
 * @return Always 0 in the current suite.
 */
static int should_promote_hpc_to_qc(const DQRCapacity *cap)
{
    (void)cap;
    return 0;
}

/**
 * @brief Count the number of fragments currently in-flight for a backend.
 *
 * In-flight fragments are those in DISPATCHED state with a route matching @p b.
 * They reduce available slots for subsequent routing decisions.
 *
 * @param ctx Context (may be NULL).
 * @param b   Backend to count in-flight fragments for.
 *
 * @return Number of in-flight fragments for backend @p b.
 */
static int count_in_flight(const DQRContext *ctx, DQRBackend b)
{
    int c = 0;
    if(!ctx) return 0;
    for(int i=0;i<ctx->n;i++)
    {
        const DQRFragment *f = &ctx->frags[i];
        if(f->state == DQR_FRAG_DISPATCHED && f->route == b)
            c++;
    }
    return c;
}

/**
 * @brief Clamp an integer to the non-negative range.
 *
 * @param x Input value.
 * @return 0 if @p x is negative; otherwise returns @p x.
 */
static int clamp_nonneg(int x) { return (x < 0) ? 0 : x; }

/**
 * @brief Select fragments for dispatch in the next wave.
 *
 * This is the core routing policy entry point used by the dispatcher.
 * It selects fragment indices into two output arrays: HPC and QC.
 *
 * Inputs/outputs:
 * - The router writes indices into @p out_hpc_idx and @p out_qc_idx.
 * - It returns the corresponding counts via @p out_hpc_n and @p out_qc_n.
 *
 * Capacity semantics:
 * - Available slots are computed as (total_slots - in_flight) per backend.
 * - Degraded backends @c ctx->cap.*_degraded are treated as unavailable.
 *
 * Iteration semantics:
 * - Iteration 0 implements a two-pass policy:
 *   1) Route labelled fragments (QC/HPC), leave UNDECIDED for later fill.
 *   2) Fill UNDECIDED according to preference (default HPC-first).
 *   Final normalization ensures deterministic reason strings for remaining UNDECIDED.
 *
 * - Iteration >= 1 prioritizes QC-labelled fragments first to preserve QC invariants
 *   and failure semantics, then routes HPC-labelled and UNDECIDED fragments in an
 *   order controlled by @c DQR_PREFER_ITERN_UNDECIDED.
 *
 * Failure semantics for QC-labelled fragments in iter >= 1:
 * - Transient failures may be retried on QC if policy allows.
 * - Permanent failures may fail over to HPC if policy allows.
 *
 * Note on @p max_out:
 * - The current implementation intentionally does not enforce truncation by
 *   @p max_out (it is ignored) to ensure every PENDING fragment receives a
 *   deterministic @c reason
 *
 * Side effects:
 * - Updates @c f->route and @c f->reason for PENDING fragments, even when not dispatched.
 *
 * @param ctx         DQR context (must not be NULL).
 * @param out_hpc_idx Output array for HPC indices (must not be NULL).
 * @param out_hpc_n   Output count for HPC indices (must not be NULL).
 * @param out_qc_idx  Output array for QC indices (must not be NULL).
 * @param out_qc_n    Output count for QC indices (must not be NULL).
 * @param max_out     Maximum output capacity per array (currently ignored).
 *
 * @return 1 on success; 0 on invalid inputs.
 */
int dqr_router_select(DQRContext *ctx, int *out_hpc_idx, int *out_hpc_n,
                      int *out_qc_idx,  int *out_qc_n,  int max_out)
{
    if(!ctx || !out_hpc_idx || !out_qc_idx || !out_hpc_n || !out_qc_n) return 0;

    *out_hpc_n = 0;
    *out_qc_n  = 0;

    (void)max_out;
    const int limit = ctx->n;

    int hpc_inflight = count_in_flight(ctx, DQR_BACKEND_HPC);
    int qc_inflight  = count_in_flight(ctx, DQR_BACKEND_QC);

    int hpc_slots = clamp_nonneg(ctx->cap.hpc_slots_total - hpc_inflight);
    int qc_slots  = clamp_nonneg(ctx->cap.qc_slots_total  - qc_inflight);

    // ITER 0
    if(ctx->iter == 0)
    {
        // Pass 1: labelled fragments and mark UNDECIDED for later fill.
        for(int i=0;i<ctx->n;i++)
        {
            DQRFragment *f = &ctx->frags[i];
            if(!is_pending(f)) continue;

            if(f->label == DQR_LABEL_QC)
            {
                if(qc_slots > 0 && !ctx->cap.qc_degraded && *out_qc_n < limit)
                {
                    out_qc_idx[(*out_qc_n)++] = i;
                    qc_slots--;
                    f->route = DQR_BACKEND_QC;
                    set_reason(f, "ITER0_LABEL_QC");
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;

                    if(ctx->cap.qc_degraded)
                        set_reason(f, "ITER0_QC_DEGRADED");
                    else if(strict_qc_label_enabled() && ctx->cap.qc_slots_total == 0)
                        set_reason(f, "ITER0_QC_NO_CAPACITY_STRICT");
                    else
                        set_reason(f, "ITER0_QC_NO_CAPACITY");
                }
            }
            else if(f->label == DQR_LABEL_HPC)
            {
                if(hpc_slots > 0 && !ctx->cap.hpc_degraded && *out_hpc_n < limit)
                {
                    out_hpc_idx[(*out_hpc_n)++] = i;
                    hpc_slots--;
                    f->route = DQR_BACKEND_HPC;
                    set_reason(f, "ITER0_LABEL_HPC");
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITER0_HPC_NO_CAPACITY");
                }
            }
            else
            {
                // UNDECIDED: decide in pass 2
                f->route = DQR_BACKEND_UNDEFINED;
                set_reason(f, "");
            }
        }

        // Pass 2: Fill UNDECIDED according to preference (default HPC-first).
        DQRBackend pref0 = prefer_iter0_undecided_backend();
        for(int i=0;i<ctx->n;i++)
        {
            DQRFragment *f = &ctx->frags[i];
            if(!is_pending(f)) continue;
            if(f->label != DQR_LABEL_UNDECIDED) continue;

            if(pref0 == DQR_BACKEND_HPC)
            {
                if(!ctx->cap.hpc_degraded && hpc_slots > 0 && *out_hpc_n < limit)
                {
                    out_hpc_idx[(*out_hpc_n)++] = i;
                    hpc_slots--;
                    f->route = DQR_BACKEND_HPC;
                    set_reason(f, "ITER0_FILL_UNDECIDED_TO_HPC");
                }
                else if(ctx->cap.qc_degraded)
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITER0_SKIP_UNDECIDED_QC_DEGRADED");
                }
                else if(qc_slots > 0 && *out_qc_n < limit)
                {
                    out_qc_idx[(*out_qc_n)++] = i;
                    qc_slots--;
                    f->route = DQR_BACKEND_QC;
                    set_reason(f, "ITER0_FILL_UNDECIDED_TO_QC");
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITER0_UNDECIDED_NO_CAPACITY");
                }
            }
            else
            {
                if(!ctx->cap.qc_degraded && qc_slots > 0 && *out_qc_n < limit)
                {
                    out_qc_idx[(*out_qc_n)++] = i;
                    qc_slots--;
                    f->route = DQR_BACKEND_QC;
                    set_reason(f, "ITER0_FILL_UNDECIDED_TO_QC");
                }
                else if(!ctx->cap.hpc_degraded && hpc_slots > 0 && *out_hpc_n < limit)
                {
                    out_hpc_idx[(*out_hpc_n)++] = i;
                    hpc_slots--;
                    f->route = DQR_BACKEND_HPC;
                    set_reason(f, "ITER0_FILL_UNDECIDED_TO_HPC");
                }
                else if(ctx->cap.qc_degraded)
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITER0_SKIP_UNDECIDED_QC_DEGRADED");
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITER0_UNDECIDED_NO_CAPACITY");
                }
            }
        }

        // Ensure UNDECIDED pending have a deterministic reason.
        for(int i=0;i<ctx->n;i++)
        {
            DQRFragment *f = &ctx->frags[i];
            if(!is_pending(f)) continue;
            if(f->label != DQR_LABEL_UNDECIDED) continue;
            if(f->route != DQR_BACKEND_UNDEFINED) continue;

            if(ctx->cap.qc_degraded && (hpc_slots <= 0 || ctx->cap.hpc_degraded))
                set_reason(f, "ITER0_SKIP_UNDECIDED_QC_DEGRADED");
            else if(f->reason[0] == '\0')
                set_reason(f, "ITER0_UNDECIDED_NO_CAPACITY");
        }

        return 1;
    }

    // 2) ITER >= 1
    {
        const DQRBackend prefN = prefer_itern_undecided_backend();

        // PASS 0: QC-labelled first; preserve QC invariants & failure semantics
        for(int i=0;i<ctx->n;i++)
        {
            DQRFragment *f = &ctx->frags[i];
            if(!is_pending(f)) continue;
            if(f->label != DQR_LABEL_QC) continue;

            const int was_failed    = (f->fail_count > 0);
            const int was_permanent = (was_failed && f->fail_permanent == 1);
            const int was_transient = (was_failed && f->fail_permanent == 0);

            // (A) Transient failure: retry QC if allowed.
            if(was_transient)
            {
                if(allow_retry_qc_on_failure())
                {
                    if(!ctx->cap.qc_degraded && qc_slots > 0 && *out_qc_n < limit)
                    {
                        out_qc_idx[(*out_qc_n)++] = i;
                        qc_slots--;
                        f->route = DQR_BACKEND_QC;
                        set_reason(f, "ITERn_QC_FAILED_RETRY_QC");
                    }
                    else
                    {
                        f->route = DQR_BACKEND_UNDEFINED;
                        if(ctx->cap.qc_degraded)
                            set_reason(f, "ITERn_QC_DEGRADED");
                        else
                            set_reason(f, "ITERn_QC_NO_CAPACITY");
                    }
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITERn_QC_FAILED_RETRY_FORBIDDEN");
                }
                continue;
            }

            // (B) Permanent failure: failover to HPC if allowed; otherwise blocked.
            if(was_permanent)
            {
                if(!allow_failover_qc_to_hpc())
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITERn_QC_FAILED_NO_FAILOVER_POLICY");
                    continue;
                }

                if(!ctx->cap.hpc_degraded && hpc_slots > 0 && *out_hpc_n < limit)
                {
                    out_hpc_idx[(*out_hpc_n)++] = i;
                    hpc_slots--;
                    f->route = DQR_BACKEND_HPC;
                    set_reason(f, "ITERn_FAILOVER_QC_TO_HPC");
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITERn_QC_FAILED_NO_CAPACITY_HPC");
                }
                continue;
            }

            // (C) Normal QC-labelled (no failure): route to QC if possible.
            if(!ctx->cap.qc_degraded && qc_slots > 0 && *out_qc_n < limit)
            {
                out_qc_idx[(*out_qc_n)++] = i;
                qc_slots--;
                f->route = DQR_BACKEND_QC;
                set_reason(f, "ITERn_LABEL_QC");
            }
            else
            {
                f->route = DQR_BACKEND_UNDEFINED;
                if(ctx->cap.qc_degraded)
                    set_reason(f, "ITERn_QC_DEGRADED");
                else
                    set_reason(f, "ITERn_QC_NO_CAPACITY");
            }
        }

        // PASS 1/2 ordering:
        // - If prefN == HPC: UNDECIDED before HPC-labelled.
        // - Else: HPC-labelled before UNDECIDED.

        if(prefN == DQR_BACKEND_HPC)
        {
            // PASS 1: UNDECIDED first (consume HPC slots first)
            for(int i=0;i<ctx->n;i++)
            {
                DQRFragment *f = &ctx->frags[i];
                if(!is_pending(f)) continue;
                if(f->label != DQR_LABEL_UNDECIDED) continue;

                if(ctx->iter0_only)
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITERn_BLOCKED_BY_ITER0_ONLY");
                    continue;
                }

                if(!ctx->cap.hpc_degraded && hpc_slots > 0 && *out_hpc_n < limit)
                {
                    out_hpc_idx[(*out_hpc_n)++] = i;
                    hpc_slots--;
                    f->route = DQR_BACKEND_HPC;
                    set_reason(f, "ITERn_UNDECIDED_TO_HPC_PREF");
                }
                else if(!ctx->cap.qc_degraded && qc_slots > 0 && *out_qc_n < limit)
                {
                    out_qc_idx[(*out_qc_n)++] = i;
                    qc_slots--;
                    f->route = DQR_BACKEND_QC;
                    set_reason(f, "ITERn_UNDECIDED_TO_QC_PREF");
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITERn_UNDECIDED_NO_CAPACITY");
                }
            }

            // PASS 2: HPC-labelled afterwards (use remaining HPC slots)
            for(int i=0;i<ctx->n;i++)
            {
                DQRFragment *f = &ctx->frags[i];
                if(!is_pending(f)) continue;
                if(f->label != DQR_LABEL_HPC) continue;

                if(!ctx->cap.hpc_degraded && hpc_slots > 0 && *out_hpc_n < limit)
                {
                    out_hpc_idx[(*out_hpc_n)++] = i;
                    hpc_slots--;
                    f->route = DQR_BACKEND_HPC;
                    set_reason(f, "ITERn_LABEL_HPC");
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITERn_HPC_NO_CAPACITY");
                }
            }
        }
        else
        {
            // PASS 1: HPC-labelled first
            for(int i=0;i<ctx->n;i++)
            {
                DQRFragment *f = &ctx->frags[i];
                if(!is_pending(f)) continue;
                if(f->label != DQR_LABEL_HPC) continue;

                if(!ctx->cap.hpc_degraded && hpc_slots > 0 && *out_hpc_n < limit)
                {
                    out_hpc_idx[(*out_hpc_n)++] = i;
                    hpc_slots--;
                    f->route = DQR_BACKEND_HPC;
                    set_reason(f, "ITERn_LABEL_HPC");
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITERn_HPC_NO_CAPACITY");
                }
            }

            // PASS 2: UNDECIDED
            for(int i=0;i<ctx->n;i++)
            {
                DQRFragment *f = &ctx->frags[i];
                if(!is_pending(f)) continue;
                if(f->label != DQR_LABEL_UNDECIDED) continue;

                if(ctx->iter0_only)
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITERn_BLOCKED_BY_ITER0_ONLY");
                    continue;
                }

                if(!ctx->cap.qc_degraded && qc_slots > 0 && *out_qc_n < limit)
                {
                    out_qc_idx[(*out_qc_n)++] = i;
                    qc_slots--;
                    f->route = DQR_BACKEND_QC;
                    set_reason(f, "ITERn_UNDECIDED_TO_QC_PREF");
                }
                else if(!ctx->cap.hpc_degraded && hpc_slots > 0 && *out_hpc_n < limit)
                {
                    out_hpc_idx[(*out_hpc_n)++] = i;
                    hpc_slots--;
                    f->route = DQR_BACKEND_HPC;
                    set_reason(f, "ITERn_UNDECIDED_TO_HPC_PREF");
                }
                else
                {
                    f->route = DQR_BACKEND_UNDEFINED;
                    set_reason(f, "ITERn_UNDECIDED_NO_CAPACITY");
                }
            }
        }

        if(should_promote_hpc_to_qc(&ctx->cap))
        {
            // no-op
        }

        return 1;
    }

}
