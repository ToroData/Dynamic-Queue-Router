/**
 * @file dqr.c
 * @brief Utilities for DQR runtime diagnostics and trace emission.
 *
 * Provides best-effort trace export helpers for DQR execution contexts.
 *
 * Intended for post-mortem analysis and debugging.
 */
#include "dqr_internal.h"
#include <stdio.h>
#include <string.h>

/**
 * @brief Write a CSV trace snapshot of the current DQR context.
 *
 * The function emits a CSV file named `dqr_trace_iter_<iter>.csv` into the
 * provided job directory.
 *
 * This routine is designed to be best-effort:
 * - If any required input is invalid (null pointers, empty directory), it
 *   returns 0 and performs no I/O.
 * - If the file cannot be opened, it returns 0.
 * - Otherwise, it returns 1 after writing and closing the file.
 *
 * Iteration handling:
 * - If @p iter is negative, the function uses the current @c ctx->iter
 *   value for the CSV "iter" column.
 * - The filename still uses the @p iter argument as provided, which preserves caller 
 *   intent.
 *
 * @param ctx     Pointer to an initialized DQR context (NOT NULL).
 * @param job_dir Output directory path for the job (NOT NULL or empty).
 * @param iter    Iteration identifier for the trace. If negative, the CSV content
 *                uses @c ctx->iter for the "iter" column.
 *
 * @return 1 on success; 0 on invalid inputs or I/O failure.
 */
int dqr_write_trace_csv(const DQRContext *ctx, const char *job_dir, int iter)
{
    if(!ctx || !job_dir || job_dir[0] == '\0') return 0;

    char path[1024];
    snprintf(path, sizeof(path), "%s/dqr_trace_iter_%d.csv", job_dir, iter);

    FILE *fp = fopen(path, "w");
    if(!fp) return 0;

    fprintf(fp, "iter,frag_id,label,route,state,reason,num_qubits,depth,two_qubit_gates\n");

    const int out_iter = (iter >= 0) ? iter : ctx->iter;

    for(int i = 0; i < ctx->n; i++)
    {
        const DQRFragment *f = &ctx->frags[i];
        fprintf(fp,
                "%d,%s,%d,%d,%d,%s,%d,%d,%d\n",
                out_iter,
                f->frag_id,
                (int)f->label,
                (int)f->route,
                (int)f->state,
                f->reason,
                f->num_qubits,
                f->depth,
                f->two_qubit_gates);
    }

    fclose(fp);
    return 1;
}
