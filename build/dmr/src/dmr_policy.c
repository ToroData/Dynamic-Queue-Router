/**
 * @file dmr_policy.c
 * @brief DMR reconfiguration policy helper code
 *
 * Contains logic used for automatically determining a reconfiguration
 * action depending on the policy requested
 */

#include "dmr.h"
#include "dmr_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

/**
 * @brief Prepare DMR to shrink or grow to the specified target and return how to reconfigure
 */
static DMRSuggestion update_resources(DMRState *dmr_state, DMRControllerState *controller_state, int new_resources)
{
    int curr_resources = dmr_get_current_node_count();
    int node_difference = abs(curr_resources - new_resources);

    if(new_resources < curr_resources)
    {
        if (!jobs_can_shrink())
        {
            int jobs_to_remove;
            if(dmr_state->is_root_process)
            {
                /*
                * If it turns out that removing no jobs is the closest match here, then this will be handled
                * in dmr_check by a call to @ref can_remove_resources, resulting in a synchronized SHOULD_STAY
                */
                jobs_to_remove = get_nearest_jobs_to_nodes(controller_state, node_difference);
                internal_set_jobs_next_shrink(controller_state, jobs_to_remove);

                debug_output("Jobs cannot shrink. Nearest jobs_to_remove match for node difference %d is %d\n", node_difference, jobs_to_remove);
            }
        }

        else if(dmr_state->is_root_process)
        {
            dmr_set_nodes_next_shrink(node_difference);
        }

        return SHOULD_SHRINK;
    }

    else if(new_resources > curr_resources)
    {
        if(dmr_state->is_root_process)
        {
            dmr_set_nodes_next_expand(node_difference);
        }

        return SHOULD_EXPAND;
    }

    else
    {
        return SHOULD_STAY;
    }
}

/** \defgroup TALP-specific logic
 *  Logic included in compilation only when TALP is available
 *  @{
 */
#if defined(COMPILED_WITH_TALP)

/**
 * @brief Get value of DMR_TALP_SENSITIVITY from environment first, then compiled default as fallback
 */
static int get_talp_sensitivity_factor(void)
{
    uint32_t sensitivity;
    bool found_from_env = get_uint32_from_env(NAMEOF(DMR_TALP_SENSITIVITY), &sensitivity);

    if (found_from_env && sensitivity < INT_MAX)
    {
        return (int)sensitivity;
    }

    return DMR_TALP_SENSITIVITY;
}

/**
 * @brief Calculate the number of resources to add or remove to approach the target efficiency,
 * ensuring the number of resources stays within the specified limits.
 *
 * @param target_efficiency The desired efficiency.
 * @param current_efficiency The current efficiency.
 * @param current_resources The current number of nodes.
 * @param min_resources The minimum number of nodes.
 * @param max_resources The maximum number of nodes.
 *
 * @return The adjusted number of resources.
 */
static int adaptive_linear_function(float target_efficiency, float current_efficiency, int current_resources, int min_resources, int max_resources)
{
    int sensitivity = get_talp_sensitivity_factor();

    // If above target, we end up with positive value, so positive change
    // If below target, we get a negative value, so negative change
    // If same as target, get 0
    int change = (int)round((target_efficiency - current_efficiency) * -sensitivity);

    int new_resources = current_resources + change;

    if (new_resources < min_resources)
    {
        new_resources = min_resources;
    }

    else if (new_resources > max_resources)
    {
        new_resources = max_resources;
    }

    return new_resources;
}

/**
 * @brief Get value of DMR_TALP_TARGET_CE from environment first, then compiled default as fallback
 */
static double get_talp_target_ce(void)
{
    double target_ce;
    bool found_from_env = get_double_from_env(NAMEOF(DMR_TALP_TARGET_CE), &target_ce);

    if (found_from_env)
    {
        return target_ce;
    }

    return DMR_TALP_TARGET_CE;
}

#endif

/**
 * @brief TALP-based policy targeting a specific communication efficiency
 * 
 * Add more resources if over target communication efficiency (CE). If under,
 * drop resources. If jobs cannot shrink, DMR will choose the configuration
 * which is closest to the ideal determined by @ref adaptive_linear_function 
 * with regard to runtime constraints.
 */
static DMRSuggestion ce_policy(DMRState *dmr_state, DMRControllerState *controller_state)
{
#if defined(COMPILED_WITH_TALP)

    TALPInfo talp_metrics = get_talp_info_acc(dmr_state, controller_state);
    double curr_efficiency = talp_metrics.communication_efficiency;

    if(curr_efficiency < 0)
    {
        if(dmr_state->is_root_process)
        {
            dmr_error("Could not get a communication efficiency reading.\n");
        }
        return SHOULD_STAY;
    }

    double target_efficiency = get_talp_target_ce();
    int curr_resources = dmr_get_current_node_count();
    int min_resources = dmr_get_policy_min_nodes();
    int max_resources = dmr_get_policy_max_nodes();
    int new_resources = adaptive_linear_function(target_efficiency, curr_efficiency, curr_resources, min_resources, max_resources);
    
    if(dmr_state->is_root_process)
    {
        debug_output("TALP_POLICY: curr_efficiency: %f, target_efficiency: %f, curr_resources: %d, min_resources: %d, max_resources: %d, new_resources: %d\n", 
        curr_efficiency, target_efficiency, curr_resources, min_resources, max_resources, new_resources);
    }

    return update_resources(dmr_state, controller_state, new_resources);
#else
    dmr_error("Not compiled with TALP, but a TALP-based policy was suggested\n");
    return SHOULD_STAY;
#endif
}

/**
 * @brief TALP-based policy targetting a specific communication efficiency
 * 
 * Add more resources if over target communication efficiency (CE). If under,
 * drop resources. If jobs cannot shrink, DMR will choose the configuration
 * which is closest to the ideal determined by @ref adaptive_linear_function 
 */
static DMRSuggestion slurm4dmr_ce_policy(DMRState *dmr_state, DMRControllerState *controller_state)
{
#if defined(SLURM4DMR) && defined(COMPILED_WITH_TALP)

if(!jobs_can_grow())
{
    if(dmr_state->is_root_process)
    {
        dmr_error("Slurm4DMR-specific policies require job growth feature to be enabled.\n");
    }
    return SHOULD_STAY;
}

int curr_resources = dmr_get_current_node_count();

TALPInfo talp_metrics = get_talp_info_acc(dmr_state, controller_state);

double curr_efficiency = talp_metrics.communication_efficiency;
float talp_etime = talp_metrics.talp_etime;

int new_resources;

if(dmr_state->is_root_process)
{
    int min_resources = dmr_get_policy_min_nodes();
    int max_resources = dmr_get_policy_max_nodes();
    int target_efficiency = get_talp_target_ce();
    int adaptive_linear_func = adaptive_linear_function(target_efficiency, curr_efficiency, curr_resources, min_resources, max_resources);

    // Handled by the custom resource manager, Slurm4DMR
    new_resources = slurm4dmr_get_talp_policy_nodes(controller_state->main_jobid, min_resources, max_resources, curr_efficiency, target_efficiency, curr_resources, talp_etime, adaptive_linear_func);
}

MPI_Bcast(&new_resources, 1, MPI_INT, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD);

return update_resources(dmr_state, controller_state, new_resources);

#else
    dmr_error("Either Slurm4DMR or TALP was not found, but the suggested policy requires both.\n");
    return SHOULD_STAY;
#endif
}
/** @} */  // End of TALP-specific logic

/**
 * @brief Resolve QCut job output directory.
 *
 * Resolution order:
 *  1) DMR_QCUT_JOB_DIR
 *  2) DMR_QCUT_OUTPUT_DIR
 */
static const char *get_qcut_job_dir(void)
{
    const char *p = getenv("DMR_QCUT_JOB_DIR");
    if(p && p[0] != '\0') return p;

    p = getenv("DMR_QCUT_OUTPUT_DIR");
    if(p && p[0] != '\0') return p;

    return NULL;
}

static int get_int_env_default(const char *name, int defv)
{
    const char *v = getenv(name);
    if(!v || v[0] == '\0') return defv;
    return atoi(v);
}

static int get_int_env_default_local(const char *name, int defv)
{
    const char *v = getenv(name);
    if(!v || !v[0]) return defv;
    return atoi(v);
}

static double get_double_env_default_local(const char *name, double defv)
{
    const char *v = getenv(name);
    if(!v || !v[0]) return defv;
    return atof(v);
}

static int dmr_str_ieq_local(const char *a, const char *b)
{
    if(!a || !b) return 0;
    while(*a && *b)
    {
        if(tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int get_iter_from_env(void)
{
    return get_int_env_default("DMR_RECONFIG_COUNT", 0);
}

static DMRSuggestion route_undef_stewardship_policy(DMRState *dmr_state)
{
    const char *job_dir = get_qcut_job_dir();
    const int iter = get_iter_from_env();

    const int qc_degraded = get_int_env_default("DMR_QCUT_QC_DEGRADED", 0);

    const int allow_hpc_to_qc = get_int_env_default("DMR_QCUT_ALLOW_HPC_TO_QC", 0);
    const int hpc_to_qc_after = get_int_env_default("DMR_QCUT_HPC_TO_QC_AFTER_ITER", 2);

    const int undef_to_qc_max = get_int_env_default("DMR_QCUT_UNDEF_TO_QC_MAX", 0);
    const double undef_to_qc_frac = get_double_env_default_local("DMR_QCUT_UNDEF_TO_QC_FRACTION", -1.0);

    if(!dmr_state->is_root_process)
        return SHOULD_STAY;

    if(!job_dir || job_dir[0] == '\0')
    {
        debug_output("ROUTE_UNDEF_STEWARDSHIP: job_dir not set. Skipping.\n");
        return SHOULD_STAY;
    }

    DMRQCutRow *rows = NULL;
    int n = 0;
    if(!dmr_qcut_read_labels_rows(job_dir, &rows, &n) || n <= 0)
    {
        debug_output("ROUTE_UNDEF_STEWARDSHIP: cannot read labels rows. Skipping.\n");
        return SHOULD_STAY;
    }

    DMRQCutLabelCounts counts;
    int have_counts = dmr_qcut_get_label_counts(job_dir, &counts);

    const char **routes  = (const char**)calloc((size_t)n, sizeof(char*));
    const char **reasons = (const char**)calloc((size_t)n, sizeof(char*));
    if(!routes || !reasons)
    {
        free(rows);
        free(routes);
        free(reasons);
        return SHOULD_STAY;
    }

    // total Undefined
    int undef_total = 0;
    for(int i=0;i<n;i++)
    {
        if(strcasecmp(rows[i].label, "Undefined") == 0)
            undef_total++;
    }

    int undef_to_qc = 0;
    if(iter >= 1 && !qc_degraded && undef_total > 0)
    {
        if(undef_to_qc_max > 0)
            undef_to_qc = (undef_to_qc_max < undef_total) ? undef_to_qc_max : undef_total;
        else if(undef_to_qc_frac >= 0.0)
        {
            int k = (int)lround(undef_to_qc_frac * (double)undef_total);
            if(k < 0) k = 0;
            if(k > undef_total) k = undef_total;
            undef_to_qc = k;
        }
    }

    int undef_seen = 0;

    for(int i=0;i<n;i++)
    {
        const char *lab = rows[i].label;

        if(strcasecmp(lab, "QC") == 0 || strcasecmp(lab, "QPU") == 0)
        {
            routes[i] = "QC";
            reasons[i] = "IMMUTABLE_QC";
            continue;
        }

        if(strcasecmp(lab, "HPC") == 0)
        {
            if(allow_hpc_to_qc && !qc_degraded && iter >= hpc_to_qc_after)
            {
                routes[i] = "QC";
                reasons[i] = "RECONSIDER_HPC_TO_QC";
            }
            else
            {
                routes[i] = "HPC";
                reasons[i] = "DEFAULT_HPC";
            }
            continue;
        }

        if(iter == 0)
        {
            routes[i]  = "Undefined";
            reasons[i] = "ITER0_KEEP_UNDEF";
            continue;
        }

        if(qc_degraded)
        {
            routes[i]  = "HPC";
            reasons[i] = "QC_DEGRADED";
            continue;
        }

        if(undef_seen < undef_to_qc)
        {
            routes[i]  = "QC";
            reasons[i] = "UNDEF_TO_QC";
        }
        else
        {
            routes[i]  = "HPC";
            reasons[i] = "UNDEF_TO_HPC";
        }
        undef_seen++;
    }

    const char *job_id_str = getenv("DMR_QCUT_JOB_ID_STR");
    if(!job_id_str) job_id_str = "";

    dmr_qcut_write_routes_csv(job_dir,
                          job_id_str,
                          rows,
                          n,
                          (const char *const *)routes,
                          (const char *const *)reasons,
                          iter);

    debug_output("ROUTE_UNDEF_STEWARDSHIP: iter=%d qc_degraded=%d undef_total=%d undef_to_qc=%d counts_ok=%d hpc=%d qc=%d undef=%d\n",
                 iter, qc_degraded, undef_total, undef_to_qc,
                 have_counts, counts.hpc, counts.qc, counts.undef);

    free(rows);
    free(routes);
    free(reasons);

    return SHOULD_STAY;
}

/**
 * @brief QCUT_PIPELINE_BALANCE policy.
 *
 * Purpose:
 * - Adjust node count (malleability) based on pipeline backlog composition.
 * - Uses counts from dmr_labels.csv (internal) and optional progress deltas.
 *
 * Inputs (env, optional):
 * - DMR_QCUT_PROGRESS_HPC_DELTA (int) default 0
 * - DMR_QCUT_PROGRESS_QC_DELTA  (int) default 0
 * - DMR_QCUT_BALANCE_BAND (int) default 0
 *
 * Behavior (simple MVP):
 * - If HPC backlog dominates QC by more than BAND => expand (+stride).
 * - If QC backlog dominates HPC by more than BAND => shrink (-stride).
 * - If HPC backlog >0 and progress_hpc_delta == 0 => expand (stalled).
 * - Otherwise => stay.
 */
static DMRSuggestion qcut_pipeline_balance_policy(DMRState *dmr_state, DMRControllerState *controller_state)
{
    int curr = dmr_get_current_node_count();
    int min_nodes = dmr_get_policy_min_nodes();
    int max_nodes = dmr_get_policy_max_nodes();
    int stride = dmr_get_policy_stride();
    if(stride <= 0) stride = 1;

    int new_nodes = curr;

    if(dmr_state->is_root_process)
    {
        const char *job_dir = get_qcut_job_dir();

        DMRQCutLabelCounts counts;
        int ok = dmr_qcut_get_label_counts(job_dir, &counts);

        int prog_hpc = get_int_env_default("DMR_QCUT_PROGRESS_HPC_DELTA", 0);
        int prog_qc  = get_int_env_default("DMR_QCUT_PROGRESS_QC_DELTA", 0);
        (void)prog_qc;
        int band     = get_int_env_default("DMR_QCUT_BALANCE_BAND", 0);

        if(!ok)
        {
            debug_output("QCUT_PIPELINE_BALANCE: could not read counts from CSV (job_dir=%s). Staying.\n",
                         job_dir ? job_dir : "(null)");
            new_nodes = curr;
        }
        else
        {
            int diff = counts.hpc - counts.qc;

            if(counts.hpc > 0 && prog_hpc == 0)
            {
                new_nodes = curr + stride;
            }
            else if(diff > band)
            {
                // predomine HPC => expand
                new_nodes = curr + stride;
            }
            else if(diff < -band)
            {
                // predomine QC => shrink
                new_nodes = curr - stride;
            }
            else
            {
                new_nodes = curr;
            }

            if(new_nodes < min_nodes) new_nodes = min_nodes;
            if(new_nodes > max_nodes) new_nodes = max_nodes;

            debug_output("QCUT_PIPELINE_BALANCE: hpc=%d qc=%d undef=%d prog_hpc=%d prog_qc=%d band=%d curr=%d => target=%d (min=%d max=%d stride=%d)\n",
                         counts.hpc, counts.qc, counts.undef, prog_hpc, prog_qc, band, curr, new_nodes, min_nodes, max_nodes, stride);
        }
    }

    MPI_Bcast(&new_nodes, 1, MPI_INT, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD);
    return update_resources(dmr_state, controller_state, new_nodes);
}

/**
 * @brief Policy for testing purposes. Expand by doubling to max, then shrink to min
 * 
 * Uses the values from dmr_get_policy_min_nodes and dmr_get_policy_max_nodes to determine
 * boundaries. Doubles up to the set maximum and shrinks to minimum if cannot double without
 * exceeding maximum. If jobs cannot shrink, uses the SHOULD_MINIMIZE suggestion to minimize
 * regardless of the set minimum.
 */
static DMRSuggestion round_policy(DMRState *dmr_state, DMRControllerState *controller_state)
{
    int min_resources = dmr_get_policy_min_nodes();
    int max_resources = dmr_get_policy_max_nodes();
    int curr_resources = dmr_get_current_node_count();
    int stride = dmr_get_policy_stride();

    int adjusted_resources = curr_resources * stride;

    int new_resources;
    
    if(adjusted_resources <= max_resources)
    {
        new_resources = adjusted_resources;
    }
    else
    {
        new_resources = min_resources;
    }

    if(dmr_state->is_root_process)
    {
        debug_output("DMR@Jobs round policy applied. Min: %d, max: %d, stride: %d, curr: %d, new: %d\n", min_resources, max_resources, stride, curr_resources, new_resources);
    }

    return update_resources(dmr_state, controller_state, new_resources);
}

static DMRSuggestion slurm4dmr_round_policy(DMRState *dmr_state, DMRControllerState *controller_state)
{
#if defined(SLURM4DMR)

if(!jobs_can_grow())
{
    if(dmr_state->is_root_process)
    {
        dmr_error("Slurm4DMR-specific policies require job growth feature to be enabled.\n");
    }
    return SHOULD_STAY;
}

int curr_resources = dmr_get_current_node_count();

int new_resources;

if(dmr_state->is_root_process)
{
    int min_resources = dmr_get_policy_min_nodes();
    int max_resources = dmr_get_policy_max_nodes();
    int step = dmr_get_policy_stride();

    // Handled by the custom resource manager, Slurm4DMR
    new_resources = slurm4dmr_get_test_policy_nodes(curr_resources, step, min_resources, max_resources);
}

MPI_Bcast(&new_resources, 1, MPI_INT, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD);

return update_resources(dmr_state, controller_state, new_resources);

#else
    dmr_error("Not compiled with Slurm4DMR, but a Slurm4DMR-based policy was suggested\n");
    return SHOULD_STAY;
#endif
}

/**
 * @brief Policy which respects a minimum, maximum, and ideal node count
 * 
 * Try to target the ideal node count, accepting changes between the minimum
 * and maximum count depending on the cluster status.
 */
DMRSuggestion slurm4dmr_queue_policy(DMRState *dmr_state, DMRControllerState *controller_state)
{
#if defined(SLURM4DMR)

if(!jobs_can_grow())
{
    if(dmr_state->is_root_process)
    {
        dmr_error("Slurm4DMR-specific policies require job growth feature to be enabled.\n");
    }
    return SHOULD_STAY;
}

int new_resources;
if(dmr_state->is_root_process)
{
    int min_resources = dmr_get_policy_min_nodes();
    int max_resources = dmr_get_policy_max_nodes();
    int curr_resources = dmr_get_current_node_count();
    int step = dmr_get_policy_stride();
    int preference = dmr_get_policy_pref_nodes();
    new_resources = slurm4dmr_get_step_policy_nodes(controller_state->main_jobid, min_resources, max_resources, curr_resources, step, preference);
}

MPI_Bcast(&new_resources, 1, MPI_INT, PROC_ZERO, dmr_state->INTERNAL_COMM_WORLD);

return update_resources(dmr_state, controller_state, new_resources);

#else
    dmr_error("Not compiled with Slurm4DMR, but a Slurm4DMR-based policy was suggested\n");
    return SHOULD_STAY;
#endif
}

/**
 * @brief Take a given policy as a DMRSuggestion, adjust DMR configuration accordingly, and return simple suggestion
 */
DMRSuggestion process_if_policy(DMRState *dmr_state, DMRControllerState *controller_state, DMRSuggestion original_suggestion)
{
    switch(original_suggestion)
    {
        case ROUND_POLICY:
            return round_policy(dmr_state, controller_state);

        case CE_POLICY:
            return ce_policy(dmr_state, controller_state);

        case SLURM4DMR_ROUND_POLICY:
            return slurm4dmr_round_policy(dmr_state, controller_state);

        case SLURM4DMR_CE_POLICY:
            return slurm4dmr_ce_policy(dmr_state, controller_state);

        case SLURM4DMR_QUEUE_POLICY:
            return slurm4dmr_queue_policy(dmr_state, controller_state);

        case ROUTE_UNDEF_STEWARDSHIP:
            return route_undef_stewardship_policy(dmr_state);

        case QCUT_PIPELINE_BALANCE:
            return qcut_pipeline_balance_policy(dmr_state, controller_state);

        default:
            return original_suggestion;
    }
}