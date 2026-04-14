/**
 * @file qcore.h
 * @brief C API for executing QCore fragment simulations.
 */
 #pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status codes returned by QCore C API functions.
 */
typedef enum {
  QCORE_OK = 0,
  QCORE_EINVALID = 1,
  QCORE_EBACKEND = 2,
  QCORE_ERUNTIME = 3,
  QCORE_EOOM = 4
} qcore_status_t;

/**
 * @brief Execute a QCore request provided as a JSON string. Caller must call qcore_free().
 */
qcore_status_t qcore_run_json(const char* input_json,
                              char** output_json,
                              size_t* output_len);

/**
 * @brief Execute a fragment given a meta JSON file path.
 */
qcore_status_t qcore_run_meta_file(const char* meta_json_path,
                                  char** output_json,
                                  size_t* output_len);

/**
 * @brief Execute a fragment given explicit meta and QASM file paths.
 */
qcore_status_t qcore_run_paths(const char* meta_json_path,
                               const char* qasm_path,
                               char** output_json,
                               size_t* output_len);

/**
 * @brief Free memory allocated by the QCore C API.
 */
void qcore_free(void* p);

/**
 * @brief Return the QCore library version string.
 */
const char* qcore_version(void);

/**
 * @brief Execute a fragment given a meta JSON file path and an explicit route.
 *
 * Identical to qcore_run_meta_file() but accepts a route hint that is
 * injected into the request JSON as "backend_target" before dispatch:
 *   route == 1 (QC)  →  "backend_target": "qpu"
 *   route == 0 (HPC) →  "backend_target": "cpu"  (same as no hint)
 *
 * This is the preferred entry-point when the caller (e.g. DMR backend sender)
 * already knows the routing decision so QCore can select the correct backend.
 *
 * @param meta_json_path Path to *.meta.json
 * @param route          DMRRoute value: 0=HPC, 1=QC
 * @param output_json    Allocated NUL-terminated JSON string (caller must free)
 * @param output_len     Length in bytes excluding NUL
 */
qcore_status_t qcore_run_meta_file_ex(const char* meta_json_path,
                                       int route,
                                       char** output_json,
                                       size_t* output_len);

#ifdef __cplusplus
}
#endif
