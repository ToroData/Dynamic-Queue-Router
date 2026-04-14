// dmr/include/dqr_slurm.h
#ifndef DQR_SLURM_H
#define DQR_SLURM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int nnodes_alloc;
    int ntasks_alloc;
    int ntasks_per_node;
} DQRSlurmAlloc;


int dqr_slurm_query_allocation(uint32_t job_id, DQRSlurmAlloc *out);

#ifdef __cplusplus
}
#endif

#endif
