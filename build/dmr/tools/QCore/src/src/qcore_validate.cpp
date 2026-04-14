/**
 * @file qcore_validate.cpp
 * @brief Meta JSON validation for QCore fragment execution.
 */
#include <nlohmann/json.hpp>
#include <string>

namespace qcore {
/**
* @brief Result of validating a fragment meta JSON object.
*
* This structure is used by @ref validate_fragment_meta to report whether
* a meta JSON object is acceptable for execution and, if not, why.
*
* @note When @c ok is false, @c message is expected to be a short, user-facing
*       explanation. @c details may carry structured diagnostics.
*/
struct Validation {
  bool ok;
  std::string message;
  nlohmann::json details;
};

/**
 * @brief Check whether a JSON value is a boolean.
 *
 * @param j JSON value.
 * @return True if @p j is a JSON boolean; false otherwise.
 */
static inline bool json_is_bool(const nlohmann::json& j) { return j.is_boolean(); }

/**
 * @brief Validate a fragment meta JSON object.
 *
 * This function validates that @p in conforms to the expected meta schema for
 * the current QCore execution path.
 *
 * Contract
 * - QASM is provided strictly via a file path in the data-plane key `"qasm_path"`.
 * - Inline/embedded QASM text inside the JSON is not part of this contract.
 * - Validation is intentionally cheap and deterministic: it does not touch the filesystem.
 *
 * Required fields
 * The following keys must exist and have the specified types:
 * - `"schema_version"` (string), must equal `"qcut.fragment.meta.v1"`
 * - `"job_id"` (string)
 * - `"fragment_id"` (string)
 * - `"export_format"` (string)
 * - `"qasm_version"` (int), must be 2 or 3
 * - `"n_qubits"` (int), must be >= 1
 * - `"depth"` (int)
 * - `"two_qubit_gates"` (int)
 * - `"qasm_path"` (string), path to a readable QASM file
 *
 * ## Optional fields
 * - `"gate_cut"` (boolean) if present.
 *
 * @param in Meta JSON object.
 * @return A @ref Validation structure:
 *         - @c ok=true if valid
 *         - @c ok=false with @c message and optional @c details if invalid.
 *
 * @note This function does not read @c qasm_path or validate its existence.
 *       File I/O is performed later by the selected backend.
 */
Validation validate_fragment_meta(const nlohmann::json& in) {
  Validation v{true, "", nlohmann::json::object()};

  auto req_str = [&](const char* k) {
    if (!v.ok) return;
    if (!in.contains(k) || !in[k].is_string()) {
      v.ok = false;
      v.message = std::string("Missing or non-string field: ") + k;
    }
  };
  auto req_int = [&](const char* k) {
    if (!v.ok) return;
    if (!in.contains(k) || !in[k].is_number_integer()) {
      v.ok = false;
      v.message = std::string("Missing or non-integer field: ") + k;
    }
  };

  req_str("schema_version");
  req_str("job_id");
  req_str("fragment_id");
  req_str("export_format");
  req_int("qasm_version");
  req_int("n_qubits");
  req_int("depth");
  req_int("two_qubit_gates");
  req_str("qasm_path");

  if (!v.ok) return v;

  if (in["schema_version"] != "qcut.fragment.meta.v1") {
    v.ok = false;
    v.message = "Unsupported schema_version";
    v.details = {{"expected", "qcut.fragment.meta.v1"}, {"got", in["schema_version"]}};
    return v;
  }

  const int qv = in["qasm_version"].get<int>();
  if (qv != 2 && qv != 3) {
    v.ok = false;
    v.message = "Only qasm_version in {2,3} supported";
    v.details = {{"supported", {2,3}}, {"got", qv}};
    return v;
  }

  const int nq = in["n_qubits"].get<int>();
  if (nq <= 0) {
    v.ok = false;
    v.message = "n_qubits must be >= 1";
    v.details = {{"got", nq}};
    return v;
  }

  if (in.contains("gate_cut")) {
    if (!json_is_bool(in["gate_cut"])) {
      v.ok = false;
      v.message = "gate_cut must be boolean if present";
      v.details = {{"got_type", in["gate_cut"].type_name()}};
      return v;
    }
  }

  return v;
}

} // namespace qcore
