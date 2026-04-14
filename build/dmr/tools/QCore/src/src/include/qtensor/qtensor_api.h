#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QTENSOR_CUT_GATE = 0,
    QTENSOR_CUT_WIRE = 1
} qtensor_cut_type_t;

typedef struct {
    double   R;
    uint64_t base;
    uint64_t terms;
    uint64_t m;
    uint64_t n;
    int      m_inferred;
} qtensor_result_t;

/* Devuelve 0 si OK */
int qtensor_reconstruct(
    const double* ev,
    size_t        n_ev,
    qtensor_cut_type_t cut_type,
    int           number_cuts,
    int64_t       number_components,
    qtensor_result_t* out,
    char**        error_msg
);

/* Helper opcional */
int qtensor_reconstruct_json(
    const double* ev,
    size_t        n_ev,
    qtensor_cut_type_t cut_type,
    int           number_cuts,
    int64_t       number_components,
    const char*   job_id,
    const char*   fragment_id,
    char**        out_json,
    char**        error_msg
);

void qtensor_free(void* p);

#ifdef __cplusplus
}
#endif
