#include "dmr.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#include "qtensor/qtensor_api.h"


#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep(ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

int dmr_backend_sender_reconstruct_from_results(
    const char *job_dir,
    const char *results_dir,
    const char *recon_filename,
    double *out_ev,
    char **out_recon_json,
    char **out_err_json
);

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    dmr_init(argc, argv);
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    const char *job_id_str = (argc >= 2) ? argv[1] : "";

    if (rank != 0) {
        // Worker ranks
        dmr_qcut_worker_loop();
        MPI_Barrier(MPI_COMM_WORLD);
        dmr_finalize();
        MPI_Finalize();
        return 0;
    }

    // Rank 0: policy tick loop
    for (;;) {
        DMRQCutStepStatus st = dmr_qcut_step(job_id_str);

        if (st == DMR_QCUT_STEP_HAS_WORK) continue;

        if (st == DMR_QCUT_STEP_BLOCKED) {
            sleep_ms(50);
            continue;
        }

        if (st == DMR_QCUT_STEP_DONE) {
            dmr_qcut_shutdown_workers();

            const char *job_dir = getenv("DMR_QCUT_JOB_DIR");
            const char *out_dir = getenv("DMR_QCUT_OUTPUT_DIR");
            if (!out_dir || out_dir[0] == '\0') out_dir = job_dir;

            const char *recon_name = getenv("DMR_QCUT_RECON_NAME");
            if (!recon_name || recon_name[0] == '\0') recon_name = "qtensor_reconstruction.json";

            if (out_dir && out_dir[0] != '\0') {
                char results_dir[1024];
                snprintf(results_dir, sizeof(results_dir), "%s/results", out_dir);

                double ev_final = 0.0;
                char *recon_json = NULL;
                char *err_json = NULL;

                int rrc = dmr_backend_sender_reconstruct_from_results(
                    out_dir,          // job_dir/output_dir where partition.json lives
                    results_dir,      // results/frag_*.json
                    recon_name,       // output file name
                    &ev_final,
                    &recon_json,
                    &err_json
                );

                if (rrc != 0) {
                    fprintf(stderr, "[RANK0] RECON ERROR rc=%d\n", rrc);
                    if (err_json) {
                        fprintf(stderr, "[RANK0] err_json: %s\n", err_json);
                        
                        if (rrc > 0) qtensor_free(err_json);
                        else         free(err_json);
                    }
                    if (recon_json) qtensor_free(recon_json);
                } else {
                    printf("{\"expected_value\": %.16g}\n", ev_final);
                    fflush(stdout);

                    fprintf(stdout, "[RANK0] RECON OK expected_value=%.16g\n", ev_final);
                    fprintf(stdout, "[RANK0] wrote: %s/%s\n", out_dir, recon_name);

                    if (recon_json) qtensor_free(recon_json);
                }
            } else {
                fprintf(stderr, "[RANK0] RECON skipped: DMR_QCUT_OUTPUT_DIR/DMR_QCUT_JOB_DIR not set\n");
            }

            break;
        }

        // ERROR
        fprintf(stderr, "[RANK0] dmr_qcut_step ERROR\n");
        dmr_qcut_shutdown_workers();
        break;
    }

    MPI_Barrier(MPI_COMM_WORLD);
    dmr_finalize();
    MPI_Finalize();
    return 0;
}
