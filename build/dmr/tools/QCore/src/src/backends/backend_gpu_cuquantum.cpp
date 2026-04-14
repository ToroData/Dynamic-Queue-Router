#include "backends/backend_common.h"

#include <chrono>
#include <stdexcept>

namespace qcore {

BackendRunResult run_gpu_cuquantum(const nlohmann::json& meta, BackendInfo& info) {
  (void)meta;
  BackendRunResult r;

  info.selected = "cuquantum";
  info.device = "gpu";
  info.gpus     = 0;

  const auto t0 = std::chrono::steady_clock::now();

  throw std::runtime_error(
    "cuQuantum backend not implemented in this build "
    "(QCORE_HAS_CUQUANTUM=0). Recompile with QCORE_ENABLE_CUQUANTUM=ON."
  );

  return r;
}
} // namespace qcore
