#ifndef DMR_BACKEND_SENDER_H
#define DMR_BACKEND_SENDER_H

#include <mpi.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MPI tags
#define DMR_TAG_WORK  9101
#define DMR_TAG_DONE  9102
#define DMR_TAG_STOP  9103

typedef enum {
    DMR_ROUTE_HPC = 0,
    DMR_ROUTE_QC  = 1
} DMRRoute;

typedef enum {
    DMR_WORK_FRAGMENT_SIM = 0,   // qcore_run_meta_file(meta)
    DMR_WORK_RECONSTRUCT  = 1    // (reserved) reconstruction orchestration
} DMRWorkKind;

typedef enum {
    DMR_COMP_DONE = 0,
    DMR_COMP_FAIL_TRANSIENT = 1,
    DMR_COMP_FAIL_PERMANENT = 2
} DMRCompletionStatus;

#ifndef DMR_MAX_FRAG_ID
#define DMR_MAX_FRAG_ID 64
#endif

#ifndef DMR_MAX_PATH
#define DMR_MAX_PATH 512
#endif

#ifndef DMR_MAX_RESULT_JSON

#define DMR_MAX_RESULT_JSON 4096
#endif

typedef struct {
    char frag_id[DMR_MAX_FRAG_ID];
    int route;                  // DMRRoute
    int kind;                   // DMRWorkKind
    int status;                 // DMRCompletionStatus
    int t_ms;                   // wall time aproximado del work (ms)
    double expected_value;      // parsed from QCore response if available
    char result_json[DMR_MAX_RESULT_JSON]; // QCore JSON response
} DMRCompletion;

typedef struct {
    MPI_Comm comm;
    int world_rank;
    int world_size;

    int *worker_busy;

    char job_dir[DMR_MAX_PATH];
    char results_dir[DMR_MAX_PATH];

    // Controls
    int qc_slots;
    int enable_results_write;
} DMRBackendSender;

/**
 * @brief Initialize backend sender on ALL ranks.
 *
 * Rank 0 will allocate worker bookkeeping.
 * Ranks > 0 will receive job_dir/results_dir via broadcast.
 */
int dmr_backend_sender_init(DMRBackendSender *s, MPI_Comm comm,
                            const char *job_dir, const char *results_dir);

void dmr_backend_sender_finalize(DMRBackendSender *s);

/**
 * @brief Rank>0: blocking worker loop (waits for work, executes QCore, sends completion).
 */
void dmr_backend_worker_loop(MPI_Comm comm);

/**
 * @brief Rank 0: enqueue one fragment (HPC/QC) and try dispatch.
 *
 * Returns 1 if it was accepted into queues (even if not dispatched yet), 0 on error.
 */
int dmr_backend_sender_submit_one(DMRBackendSender *s,
                                 const char *frag_id,
                                 int route /*DMRRoute*/);

/**
 * @brief Rank 0: poll all completions currently available (non-blocking),
 *        and opportunistically dispatch queued work onto newly freed workers.
 *
 * Returns number of completions written into out[] up to max_out.
 */
int dmr_backend_sender_poll(DMRBackendSender *s,
                           DMRCompletion *out, int max_out);

/**
 * @brief Rank 0: ask workers to stop (best effort).
 */
void dmr_backend_sender_broadcast_stop(DMRBackendSender *s);

/**
 * @brief Rank 0: in-process tensor reconstruction helper (Qtensor).
 *
 * This is meant to be called once you have all expected values in order.
 * It returns:
 *  - recon_json: malloc'ed buffer owned by Qtensor -> must be freed via qtensor_free().
 *  - err_json:   malloc'ed buffer owned by Qtensor -> must be freed via qtensor_free().
 *
 * You can then persist recon_json to disk from the caller.
 *
 * Returns 0 on success, non-zero on error.
 */
int dmr_backend_sender_qtensor_reconstruct(
    const double *ev_vec,
    size_t nitems,
    int cut_type /*0=GATE,1=WIRE*/,
    int k_cuts,
    int64_t number_components,
    const char *job_id,
    char **recon_json,
    char **err_json
);

int dmr_backend_sender_reconstruct_from_results(
    const char *job_dir,
    const char *results_dir,
    const char *recon_filename,
    double *out_ev,
    char **out_recon_json,
    char **out_err_json
);

#ifdef __cplusplus
}
#endif

#endif // DMR_BACKEND_SENDER_H
