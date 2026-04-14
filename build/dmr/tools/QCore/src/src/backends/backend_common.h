/**
 * @file backend_common.h
 * @brief Backend interfaces and shared result structures.
 */
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace qcore {

/** @brief Cut type associated with a fragment result. */
enum class CutType {
  Wire = 0,
  Gate = 1,
};

/** @brief Backend execution output for a single fragment. */
struct BackendRunResult {
  double expected_value = 0.0;
  int shots = 0;
  std::string pauli;

  CutType cut_type = CutType::Wire;

  int gate_cut_index = -1;
  int gate_cut_sign = +1;

  double simulate_ms = 0.0;
  double observables_ms = 0.0;
};

/** @brief Backend selection metadata for a run. */
struct BackendInfo {
  std::string selected;
  std::string device;
  int gpus = 0;
};

/** @brief Execute a fragment on CPU using the Qulacs backend. */
BackendRunResult run_cpu_qulacs(const nlohmann::json& meta, BackendInfo& info);

/** @brief Execute a fragment on GPU using the cuQuantum backend. */
BackendRunResult run_gpu_cuquantum(const nlohmann::json& meta, BackendInfo& info);

/** @brief Execute a fragment on the real CESGA qmio QPU (via Python bridge). */
BackendRunResult run_qpu_qulacs(const nlohmann::json& meta, BackendInfo& info);

/** @brief Dispatch execution to the selected backend. */
BackendRunResult dispatch_run(const nlohmann::json& meta, BackendInfo& info);

} // namespace qcore
