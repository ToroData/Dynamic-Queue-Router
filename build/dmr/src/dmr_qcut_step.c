/**
 * @file dmr_qcut_step.c
 * @brief QCUT policy runtime tick for DMR+DQR master/worker execution.
 *
 * Rank 0 owns the DQR context, ingests labels from the job CSV once, refreshes
 * capacity each tick, dispatches fragments via DMR backend sender, and ACKs
 * completions back into DQR. Worker ranks are expected to run the backend worker
 * loop separately.
 */
#include "dmr.h"
#include "dqr.h"
#include "dmr_backend_sender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct timespec g_dqr_t0;
static int             g_dqr_t0_set = 0;
 
static double dqr_elapsed_s(void) {
    struct timespec now;
#if defined(_WIN32)
    clock_gettime(CLOCK_REALTIME, &now);
#elif defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &now);
#else
    clock_gettime(CLOCK_REALTIME, &now);
#endif
    return (double)(now.tv_sec  - g_dqr_t0.tv_sec)
         + (double)(now.tv_nsec - g_dqr_t0.tv_nsec) / 1e9;
}

#if defined(_WIN32)
#include <windows.h>
/**
 * @brief Sleep for a given number of milliseconds (Windows).
 *
 * @param ms Milliseconds to sleep.
 */
static void sleep_ms(int ms) { Sleep(ms); }
#else
#include <unistd.h>
/**
 * @brief Sleep for a given number of milliseconds (POSIX).
 *
 * @param ms Milliseconds to sleep.
 */
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

/**
 * @brief Debugging support: env var controlled debug printing for DQR/QCut runtime.
*/
static int g_dmr_qcut_dqr_debug = -1; // -1 => uninitialized

/**
 * @brief Read integer from environment variable @p key or return @p defval if unset/empty.
 *
 * @param key Environment variable name.
 * @param defval Default value to return if @p key is unset/empty.
 * @return Parsed integer value (strtol-based) or @p defval.
 */
static int env_int_or(const char *key, int defval) {
    const char *v = getenv(key);
    if (!v || !*v) return defval;
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v) return defval;
    return (int)x;
}

/**
 * @brief Return the current DQR debug level (0-2).
 * 
 * @return int Current debug level.
 */
static int dmr_qcut_dqr_debug_level(void) {
    if (g_dmr_qcut_dqr_debug < 0) {
        // 0: off, 1: transient/permanent, 2: cap/plan/backlog
        g_dmr_qcut_dqr_debug = env_int_or("DMR_QCUT_DQR_DEBUG", 0);
        if (g_dmr_qcut_dqr_debug < 0) g_dmr_qcut_dqr_debug = 0;
    }
    return g_dmr_qcut_dqr_debug;
}

/**
 * @brief Debugging macro for DQR/QCut runtime.
 * 
 * @param level Debug level (0-2).
 * @param fmt Format string.
 * @param ... Arguments for format string.
 */
#define DQRDBG(level, fmt, ...) \
    do { \
        if (dmr_qcut_dqr_debug_level() >= (level)) { \
            fprintf(stderr, fmt, ##__VA_ARGS__); \
            fflush(stderr); \
        } \
    } while (0)

// Globals for the QCut policy runtime (rank 0 only)

/** @brief Whether the QCUT runtime has been initialized (rank 0 owns state). */
static int g_qcut_inited = 0;

/** @brief DQR context owned by rank 0; NULL until initialized. */
static DQRContext *g_dqr = NULL;

/** @brief Backend sender used by rank 0 to submit and poll fragment executions. */
static DMRBackendSender g_sender;

// Helpers

/**
 * @brief Read environment variable @p k or return @p fallback if unset/empty.
 *
 * @param k Environment variable name.
 * @param fallback Value to return when @p k is unset/empty (may be NULL).
 * @return Environment value pointer, or @p fallback.
 */
static const char* env_or(const char *k, const char *fallback) {
    const char *v = getenv(k);
    return (v && v[0]) ? v : fallback;
}

/**
 * @brief Parse textual label into DQR label enum value.
 *
 * Accepted values: "HPC", "QC", "UNDECIDED". Unknown values map to UNDECIDED.
 *
 * @param s Label string (may be NULL).
 * @return DQR label value.
 */
static int parse_label(const char *s) {
    if (!s) return DQR_LABEL_UNDECIDED;
    if (strcmp(s, "HPC") == 0) return DQR_LABEL_HPC;
    if (strcmp(s, "QC") == 0) return DQR_LABEL_QC;
    if (strcmp(s, "UNDECIDED") == 0) return DQR_LABEL_UNDECIDED;
    return DQR_LABEL_UNDECIDED;
}

/**
 * @brief Ingest labels CSV into DQR fragments (one-time per process lifetime).
 *
 * Reads <job_dir>/dmr_labels.csv and adds fragments to @p ctx. This helper
 * resets existing fragments via dqr_reset_fragments() before ingestion.
 *
 * CSV header is assumed to include: job_id,subcircuit,label,num_qubits,depth,
 * two_qubit_gates,source_json (quoted commas not supported).
 *
 * @param ctx DQR context to populate.
 * @param job_dir QCUT job directory containing dmr_labels.csv.
 * @return 1 on success, 0 on failure (missing file, invalid args, I/O errors).
 */
static int qcut_ingest_labels_once(DQRContext *ctx, const char *job_dir) {
    if (!ctx || !job_dir || !job_dir[0]) return 0;

    char path[1024];
    snprintf(path, sizeof(path), "%s/dmr_labels.csv", job_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[2048];
    int line_no = 0;

    dqr_reset_fragments(ctx);

    while (fgets(line, sizeof(line), f)) {
        line_no++;
        if (line_no == 1) continue; // header

        char *cols[8] = {0};
        int ncols = 0;
        char *p = line;
        while (ncols < 8) {
            cols[ncols++] = p;
            char *comma = strchr(p, ',');
            if (!comma) break;
            *comma = '\0';
            p = comma + 1;
        }
        if (ncols < 3) continue;

        const char *subcircuit = cols[1]; // e.g. frag_000.meta
        const char *label_s   = cols[2];  // HPC/QC/UNDECIDED

        // trim newline from last col
        for (int i = 0; i < ncols; ++i) {
            if (!cols[i]) continue;
            size_t L = strlen(cols[i]);
            while (L > 0 && (cols[i][L-1] == '\n' || cols[i][L-1] == '\r')) cols[i][--L] = '\0';
        }

        // frag_id: strip optional ".meta" suffix
        char frag_id[64];
        memset(frag_id, 0, sizeof(frag_id));
        strncpy(frag_id, subcircuit, sizeof(frag_id)-1);
        char *dot = strstr(frag_id, ".meta");
        if (dot) *dot = '\0';

        DQRFragment frag;
        memset(&frag, 0, sizeof(frag));
        strncpy(frag.frag_id, frag_id, sizeof(frag.frag_id)-1);
        frag.label = (DQRLabel)parse_label(label_s);
        frag.route = DQR_BACKEND_UNDEFINED;
        frag.state = DQR_FRAG_PENDING;

        // optional numeric columns if present
        if (ncols >= 6) {
            frag.num_qubits = atoi(cols[3]);
            frag.depth = atoi(cols[4]);
            frag.two_qubit_gates = atoi(cols[5]);
        }

        if (!dqr_add_fragment(ctx, &frag)) {
            // ignore duplicates/invalid lines
        }
    }

    fclose(f);
    return 1;
}

/**
 * @brief Initialize QCUT runtime state on rank 0 if not already initialized.
 *
 * Creates the DQR context (rank 0 only), ingests label CSV once, and initializes
 * the backend sender. Non-root ranks perform a no-op init and return success.
 *
 * Env:
 * - DMR_QCUT_JOB_DIR (required on rank 0)
 * - SLURM_JOB_ID (optional)
 * - DMR_QC_SLOTS, DMR_QC_DEGRADED, DMR_HPC_DEGRADED (optional capacity knobs)
 * - DMR_QCUT_OUTPUT_DIR (optional; defaults to job_dir)
 *
 * @return 1 on success, 0 on failure (rank 0 missing env/file/init error).
 */
static int qcut_init_rank0_if_needed(void) {
    if (g_qcut_inited) return 1;

    int rank = 0, size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (rank != 0) {
        // Only rank0 maintains DQR context.
        g_qcut_inited = 1;
        return 1;
    }

    const char *job_dir = env_or("DMR_QCUT_JOB_DIR", NULL);
    if (!job_dir) return 0;

    DQRConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.slurm_job_id = (uint32_t)env_int_or("SLURM_JOB_ID", 0);

    // Capacity: by default, HPC slots = workers (size-1).
    // QC slots comes from env (0 default).
    cfg.capacity.hpc_slots_total = (size > 1) ? (size - 1) : 0;
    cfg.capacity.qc_slots_total  = env_int_or("DMR_QC_SLOTS", 0);
    cfg.capacity.qc_degraded     = env_int_or("DMR_QC_DEGRADED", 0);
    cfg.capacity.hpc_degraded    = env_int_or("DMR_HPC_DEGRADED", 0);
    cfg.iter0_only = 0;

    g_dqr = dqr_create(&cfg);
    if (!g_dqr) return 0;

    if (!qcut_ingest_labels_once(g_dqr, job_dir)) {
        return 0;
    }

    // sender init
    const char *out_dir = env_or("DMR_QCUT_OUTPUT_DIR", job_dir);
    char results_dir[1024];
    snprintf(results_dir, sizeof(results_dir), "%s/results", out_dir);

    if (!dmr_backend_sender_init(&g_sender, MPI_COMM_WORLD, out_dir, results_dir)) {
        return 0;
    }

/* Capture monotonic origin for T= timestamps in Gantt logs */
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &g_dqr_t0);
#else
    clock_gettime(CLOCK_REALTIME, &g_dqr_t0);
#endif
    g_dqr_t0_set = 1;

    g_qcut_inited = 1;
    return 1;
}

/**
 * @brief Refresh DQR capacity from current MPI size and environment knobs.
 *
 * Rank 0 only. Updates HPC slots to (mpi_size-1) and reads QC slots/degraded
 * flags from env.
 */
static void qcut_refresh_capacity_rank0(void) {
    int rank = 0, size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (rank != 0 || !g_dqr) return;

    DQRCapacity cap;
    memset(&cap, 0, sizeof(cap));
    cap.hpc_slots_total = (size > 1) ? (size - 1) : 0;
    cap.qc_slots_total  = env_int_or("DMR_QC_SLOTS", 0);
    cap.qc_degraded     = env_int_or("DMR_QC_DEGRADED", 0);
    cap.hpc_degraded    = env_int_or("DMR_HPC_DEGRADED", 0);

    dqr_set_capacity(g_dqr, &cap);
}

/**
 * @brief Acknowledge fragment completion into DQR (rank 0 only).
 *
 * @param frag_id Fragment id to acknowledge.
 * @return 1 if acknowledged, 0 otherwise (non-root, not initialized, invalid id).
 */
int dmr_qcut_ack_fragment_done(const char *frag_id) {
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank != 0 || !g_dqr || !frag_id || !frag_id[0]) return 0;
    return dqr_ack_fragment_done(g_dqr, frag_id);
}

/**
 * @brief Acknowledge fragment failure into DQR (rank 0 only).
 *
 * @param frag_id Fragment id to acknowledge.
 * @param permanent Non-zero for permanent failure, zero for transient.
 * @return 1 if acknowledged, 0 otherwise (non-root, not initialized, invalid id).
 */
int dmr_qcut_ack_fragment_failed(const char *frag_id, int permanent) {
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank != 0 || !g_dqr || !frag_id || !frag_id[0]) return 0;
    return dqr_ack_fragment_failed(g_dqr, frag_id, permanent ? 1 : 0);
}

/**
 * @brief Execute one QCUT policy tick (rank 0 master).
 *
 * Rank 0:
 * - Initializes runtime if needed (DQR + label ingestion + backend sender).
 * - Polls completions and ACKs done/failed into DQR.
 * - Refreshes capacity from MPI size and env knobs.
 * - Builds a dispatch plan and submits fragments (HPC/QC) via backend sender.
 * - Commits into DQR only the subset actually submitted.
 *
 * Non-root ranks:
 * - Should be running the worker loop; this function returns BLOCKED.
 *
 * @param job_id_str Optional job id string (unused in this minimal implementation).
 * @return Step status indicating DONE, HAS_WORK, BLOCKED, or ERROR.
 */
DMRQCutStepStatus dmr_qcut_step(const char *job_id_str) {
    (void)job_id_str; // optional for tracing/csv naming

    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // Rank0 runs the tick; ranks>0 should be in worker loop.
    if (rank != 0) return DMR_QCUT_STEP_BLOCKED;

    if (!qcut_init_rank0_if_needed()) return DMR_QCUT_STEP_ERROR;

    // Poll completions (non-blocking) and ACK into DQR
    DMRCompletion comps[64];
    int ncomp = dmr_backend_sender_poll(&g_sender, comps, 64);
    for (int i = 0; i < ncomp; ++i) {
        if (comps[i].status == DMR_COMP_DONE) {
            dqr_ack_fragment_done(g_dqr, comps[i].frag_id);
        } else if (comps[i].status == DMR_COMP_FAIL_PERMANENT) {
            DQRDBG(1,
                    "[RANK0] T=%.3f PERMANENT_FAIL frag_id=%s route=%s result=%s\n",
                    dqr_elapsed_s(),
                    comps[i].frag_id,
                    (comps[i].route == DMR_ROUTE_QC ? "QC" : "HPC"),
                    comps[i].result_json);
            dqr_ack_fragment_failed(g_dqr, comps[i].frag_id, 1);
        } else {
            DQRDBG(1,
                    "[RANK0] T=%.3f TRANSIENT_FAIL frag_id=%s route=%s result=%s\n",
                    dqr_elapsed_s(),
                    comps[i].frag_id,
                    (comps[i].route == DMR_ROUTE_QC ? "QC" : "HPC"),
                    comps[i].result_json);
            int ack_rc = dqr_ack_fragment_failed(g_dqr, comps[i].frag_id, 0);
            if (ack_rc == 2) {
                /* transient promoted to permanent after max retries */
                DQRDBG(1,
                        "[RANK0] T=%.3f PERMANENT_FAIL frag_id=%s route=%s (max retries)\n",
                        dqr_elapsed_s(),
                        comps[i].frag_id,
                        (comps[i].route == DMR_ROUTE_QC ? "QC" : "HPC"));
            } else {
                DQRDBG(1, "[RANK0] T=%.3f REQUEUE frag_id=%s\n",
                       dqr_elapsed_s(), comps[i].frag_id);
            }
        }
    }

    // Refresh capacity (workers, qc slots, degraded flags)
    qcut_refresh_capacity_rank0();

    // Debug
    DQRCapacity cap_now;
    memset(&cap_now, 0, sizeof(cap_now));
    dqr_get_capacity(g_dqr, &cap_now);
    DQRDBG(2, "[RANK0] T=%.3f cap: hpc=%d qc=%d hpc_deg=%d qc_deg=%d\n",
       dqr_elapsed_s(),
       cap_now.hpc_slots_total, cap_now.qc_slots_total,
       cap_now.hpc_degraded, cap_now.qc_degraded);

    // Plan + commit next wave
    DQRDispatchPlan plan;
    memset(&plan, 0, sizeof(plan));

    if (!dqr_plan_next_dispatch(g_dqr, &plan)) {
        fprintf(stderr, "[RANK0] ERROR: dqr_plan_next_dispatch failed\n");
        return DMR_QCUT_STEP_ERROR;
    }

    DQRDBG(2, "[RANK0] T=%.3f plan: hpc_count=%d qc_count=%d (iter=%d)\n",
       dqr_elapsed_s(),
       plan.hpc_count, plan.qc_count, dqr_get_iter(g_dqr));

    // backlog snapshot before commit (to decide DONE/BLOCKED precisely)
    DQRBacklogCounts bc;
    memset(&bc, 0, sizeof(bc));
    dqr_get_backlog_counts(g_dqr, &bc);

    DQRDBG(2, "[RANK0] T=%.3f backlog: pend(hpc=%d qc=%d und=%d) inflight(hpc=%d qc=%d)\n",
       dqr_elapsed_s(),
       bc.hpc_pending, bc.qc_pending, bc.undecided_pending,
       bc.hpc_dispatched, bc.qc_dispatched);

    if (plan.hpc_count == 0 && plan.qc_count == 0) {
        int pending_total  = bc.hpc_pending + bc.qc_pending + bc.undecided_pending;
        int inflight_total = bc.hpc_dispatched + bc.qc_dispatched;

        dqr_free_dispatch_plan(&plan);

        if (pending_total == 0 && inflight_total == 0) {
            /* Final drain: flush any remaining completions in MPI buffer */
            DMRCompletion drain[64];
            int nd;
            while ((nd = dmr_backend_sender_poll(&g_sender, drain, 64)) > 0) {
                for (int j = 0; j < nd; ++j) {
                    if (drain[j].status == DMR_COMP_DONE) {
                        dqr_ack_fragment_done(g_dqr, drain[j].frag_id);
                    } else if (drain[j].status == DMR_COMP_FAIL_PERMANENT) {
                        DQRDBG(1, "[RANK0] T=%.3f PERMANENT_FAIL frag_id=%s route=%s result=%s\n",
                               dqr_elapsed_s(), drain[j].frag_id,
                               (drain[j].route == DMR_ROUTE_QC ? "QC" : "HPC"),
                               drain[j].result_json);
                        dqr_ack_fragment_failed(g_dqr, drain[j].frag_id, 1);
                    } else {
                        DQRDBG(1, "[RANK0] T=%.3f TRANSIENT_FAIL frag_id=%s route=%s result=%s\n",
                               dqr_elapsed_s(), drain[j].frag_id,
                               (drain[j].route == DMR_ROUTE_QC ? "QC" : "HPC"),
                               drain[j].result_json);
                        int rc = dqr_ack_fragment_failed(g_dqr, drain[j].frag_id, 0);
                        if (rc == 2) {
                            DQRDBG(1, "[RANK0] T=%.3f PERMANENT_FAIL frag_id=%s route=%s (max retries)\n",
                                   dqr_elapsed_s(), drain[j].frag_id,
                                   (drain[j].route == DMR_ROUTE_QC ? "QC" : "HPC"));
                        }
                    }
                }
            }
            return DMR_QCUT_STEP_DONE;
        }
        return DMR_QCUT_STEP_BLOCKED;
    }


    // Submit first; commit only what was actually submitted
    DQRDispatchPlan committed;
    memset(&committed, 0, sizeof(committed));
    committed.iter = dqr_get_iter(g_dqr);

    // Reserve arrays up to original plan sizes
    committed.hpc_indices = (int*)calloc((size_t)plan.hpc_count, sizeof(int));
    committed.qc_indices  = (int*)calloc((size_t)plan.qc_count,  sizeof(int));
    committed.hpc_count = 0;
    committed.qc_count  = 0;

    int submitted = 0;

    // HPC
    for (int i = 0; i < plan.hpc_count; ++i) {
        int idx = plan.hpc_indices[i];
        const DQRFragment *f = dqr_get_fragment(g_dqr, idx);
        if (!f) continue;

        if (dmr_backend_sender_submit_one(&g_sender, f->frag_id, DMR_ROUTE_HPC)) {
            DQRDBG(1, "[RANK0] T=%.3f DISPATCH hpc frag_id=%s\n",
                   dqr_elapsed_s(), f->frag_id);
            committed.hpc_indices[committed.hpc_count++] = idx;
            submitted++;
        }
    }

    // QC
    for (int i = 0; i < plan.qc_count; ++i) {
        int idx = plan.qc_indices[i];
        const DQRFragment *f = dqr_get_fragment(g_dqr, idx);
        if (!f) continue;

        if (dmr_backend_sender_submit_one(&g_sender, f->frag_id, DMR_ROUTE_QC)) {
            DQRDBG(1, "[RANK0] T=%.3f DISPATCH qc  frag_id=%s\n",
                   dqr_elapsed_s(), f->frag_id);
            committed.qc_indices[committed.qc_count++] = idx;
            submitted++;
        }
    }

    // Commit only submitted entries
    if (committed.hpc_count > 0 || committed.qc_count > 0) {
        committed.plan_id = 0;
        dqr_commit_dispatch(g_dqr, &committed);
    }

    // Cleanup
    free(committed.hpc_indices);
    free(committed.qc_indices);
    dqr_free_dispatch_plan(&plan);


    // Determine aggregate status
    memset(&bc, 0, sizeof(bc));
    dqr_get_backlog_counts(g_dqr, &bc);

    int pending_total = bc.hpc_pending + bc.qc_pending + bc.undecided_pending;
    int inflight_total = bc.hpc_dispatched + bc.qc_dispatched;

    // if (pending_total == 0 && inflight_total == 0) return DMR_QCUT_STEP_DONE;
    if (pending_total == 0 && inflight_total == 0) {
        /* Final drain: flush any remaining completions in MPI buffer */
        DMRCompletion drain[64];
        int nd;
        while ((nd = dmr_backend_sender_poll(&g_sender, drain, 64)) > 0) {
            for (int j = 0; j < nd; ++j) {
                if (drain[j].status == DMR_COMP_DONE) {
                    dqr_ack_fragment_done(g_dqr, drain[j].frag_id);
                }
            }
        }
        return DMR_QCUT_STEP_DONE;
    }
    if (submitted > 0) return DMR_QCUT_STEP_HAS_WORK;

    // No submissions this tick:
    // - if in-flight exists, we are blocked waiting completions
    // - if pending exists but nothing dispatchable (capacity/labels/degraded), also blocked
    return DMR_QCUT_STEP_BLOCKED;
}
