/**
 * @file backend_dispatch.cpp
 * @brief Backend selection and dispatch logic.
 */
#include "backends/backend_common.h"

#if QCORE_HAS_CUQUANTUM
  #include <cuda_runtime.h>
#endif

namespace qcore {
/**
 * @brief Detect GPU availability at runtime.
 */
static bool gpu_available_runtime(int& ngpu) {
  ngpu = 0;
#if QCORE_HAS_CUQUANTUM
  int n = 0;
  if (cudaGetDeviceCount(&n) != cudaSuccess) return false;
  if (n <= 0) return false;
  ngpu = n;
  return true;
#else
  (void)ngpu;
  return false;
#endif
}

/**
 * @brief Dispatch execution to the appropriate backend.
 *
 * The optional "backend_target" field in the meta JSON allows you to force the following backend:
 * "qpu" → Real QPU CESGA qmio (via Python bridge)
 * "gpu" → cuQuantum GPU
 * "cpu" → Qulacs CPU (default)
 *
 * If the field is not present, the selection follows the logic of
 * automatic hardware detection (GPU → CPU).
 */
BackendRunResult dispatch_run(const nlohmann::json& meta, BackendInfo& info) {
  const std::string target = meta.value("backend_target", "");

  if (target == "qpu") {
    info.selected = "qpu_qulacs";
    info.device   = "qpu";
    info.gpus     = 0;
    return run_qpu_qulacs(meta, info);
  }

  if (target == "gpu") {
    int ngpu = 0;
    (void)gpu_available_runtime(ngpu);
    info.selected = "cuquantum";
    info.device   = "gpu";
    info.gpus     = ngpu;
    return run_gpu_cuquantum(meta, info);
  }

  info.selected = "qulacs";
  info.device   = "cpu";
  info.gpus     = 0;
  return run_cpu_qulacs(meta, info);
}

} // namespace qcore
