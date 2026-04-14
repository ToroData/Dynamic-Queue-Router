/**
 * @file qtensor.h
 * @brief Public API for tensor-based reconstruction utilities.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace qtensor {

/**
 * @brief Type of circuit cut used during reconstruction.
 */
enum class CutType : std::uint8_t { GATE = 0, WIRE = 1 };

/**
 * @brief Options controlling the reconstruction process and JSON output.
 */
struct ReconstructOptions {
  CutType cut_type = CutType::GATE;
  int number_cuts = 0;                 // k
  std::int64_t number_components = -1; // m. if <0 => infer
  std::string output_dir;
  std::string output_filename = "qtensor_reconstruction.json";

  std::string job_id;
  std::string fragment_id;
};

/**
 * @brief Result of a reconstruction computation.
 */
struct ReconstructResult {
  double R = 0.0;
  std::uint64_t base = 0;
  std::uint64_t terms = 0;
  std::uint64_t m = 0;
  std::uint64_t n = 0;
  bool m_inferred = false;
};

/**
 * @brief Reconstruct a scalar value from expectation values.
 */
ReconstructResult reconstruct(const std::vector<double>& ev,
                              CutType cut_type,
                              int number_cuts,
                              std::int64_t number_components = -1);

/**
 * @brief Perform reconstruction and return a JSON representation of the result.
 */
nlohmann::json reconstruct_json(const std::vector<double>& ev,
                                const ReconstructOptions& opt);

} // namespace qtensor
