/**
 * @file qcore_result.cpp
 * @brief JSON response builders for QCore execution results.
 */
#include "backends/backend_common.h"
#include <nlohmann/json.hpp>

namespace qcore {

/**
 * @brief Attach common identifiers from the request into the response.
 */
static inline void attach_ids(nlohmann::json& out, const nlohmann::json& in) {
  out["job_id"] = in.value("job_id", "");
  out["fragment_id"] = in.value("fragment_id", "");
}

/**
 * @brief Convert cut type enum to its JSON string representation.
 */
static inline const char* cut_type_to_string(CutType ct) {
  return (ct == CutType::Gate) ? "gate" : "wire";
}

/**
 * @brief Build an "ok" JSON response for a successful backend run.
 */
nlohmann::json make_ok_result(const nlohmann::json& in,
                             const BackendInfo& bi,
                             const BackendRunResult& br,
                             double parse_ms,
                             double total_ms)
{
  nlohmann::json out;
  out["schema_version"] = "qcore.result.v1";
  out["status"] = "ok";
  attach_ids(out, in);

  out["backend"] = {
    {"selected", bi.selected},
    {"device", bi.device},
    {"gpus", bi.gpus}
  };

  out["timing_ms"] = {
    {"parse", parse_ms},
    {"simulate", br.simulate_ms},
    {"observables", br.observables_ms},
    {"total", total_ms}
  };

  out["results"] = {
    {"expected_value", br.expected_value},
    {"shots", br.shots},
    {"pauli", br.pauli}
  };

  nlohmann::json cut;
  cut["type"] = cut_type_to_string(br.cut_type);

  if (br.cut_type == CutType::Gate) {
    if (br.gate_cut_index >= 0) {
      cut["term_index"] = br.gate_cut_index;
      cut["term_sign"]  = br.gate_cut_sign; // +1 or -1
    }
  }
  out["cut"] = cut;

  out["diagnostics"] = {
    {"warnings", nlohmann::json::array()},
    {"info", nlohmann::json::array()}
  };

  return out;
}

/**
 * @brief Build an "error" JSON response for a failed run.
 */
nlohmann::json make_error_result(const nlohmann::json& in,
                                 const std::string& code,
                                 const std::string& message,
                                 const nlohmann::json& details)
{
  nlohmann::json out;
  out["schema_version"] = "qcore.result.v1";
  out["status"] = "error";
  attach_ids(out, in);

  out["error"] = {
    {"code", code},
    {"message", message},
    {"details", details}
  };

  out["diagnostics"] = {
    {"warnings", nlohmann::json::array()},
    {"info", nlohmann::json::array()}
  };

  return out;
}

} // namespace qcore
