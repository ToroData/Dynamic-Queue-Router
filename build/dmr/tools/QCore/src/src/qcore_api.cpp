/**
 * @file qcore_api.cpp
 * @brief C API entrypoints for running QCore fragment simulations.
 *
 * This translation unit implements the exported C interface declared in @ref qcore/qcore.h.
 *
 * @details
 * QCore uses a strict file-based invocation model:
 * - The caller provides a fragment meta JSON file (e.g., `frag_000.meta.json`).
 * - The paired QASM file is inferred strictly as `frag_000.qasm` by replacing the
 *   `.meta.json` suffix with `.qasm` in the same directory.
 * - The meta JSON is parsed and the request is formed by attaching the QASM file path
 *   under the `"qasm_path"` key (data-plane: path, not embedded QASM text).
 * - The request is validated and dispatched to the selected backend (CPU/Qulacs, GPU/cuQuantum, ...).
 * - A JSON response is returned through an allocated buffer that must be released
 *   with @ref qcore_free.
 *
 */

 #include "qcore/qcore.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "backends/backend_common.h"

namespace qcore {
  struct Validation { bool ok; std::string message; nlohmann::json details; };
  Validation validate_fragment_meta(const nlohmann::json& in);

  nlohmann::json make_ok_result(const nlohmann::json& in,
                               const BackendInfo& bi,
                               const BackendRunResult& br,
                               double parse_ms,
                               double total_ms);

  nlohmann::json make_error_result(const nlohmann::json& in,
                                  const std::string& code,
                                  const std::string& message,
                                  const nlohmann::json& details);
}

/**
 * @brief Return milliseconds between two steady_clock instants.
 */
static inline double ms(const std::chrono::steady_clock::time_point& a,
                        const std::chrono::steady_clock::time_point& b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

/**
 * @brief Read the entire file contents into a string.
 *
 * @param path Path to a readable file.
 * @return File contents as a string.
 * @throws std::runtime_error If the file cannot be opened.
 *
 * @note The file is read in binary mode and streamed into memory.
 */
static std::string slurp_file_or_throw(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot open file: " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/**
 * @brief Infer the paired QASM path from a meta JSON path.
 *
 * The inference is strict:
 *   - input:  `<prefix>.meta.json`
 *   - output: `<prefix>.qasm`
 *
 * @param meta_path Path to the meta JSON file.
 * @return Inferred QASM path.
 * @throws std::runtime_error If @p meta_path does not end with `.meta.json`.
 */
static std::string infer_qasm_path_from_meta_path_or_throw(const std::string& meta_path) {
  const std::string suffix = ".meta.json";
  if (meta_path.size() < suffix.size() ||
      meta_path.compare(meta_path.size() - suffix.size(), suffix.size(), suffix) != 0) {
    throw std::runtime_error("meta_json_path must end with '.meta.json': " + meta_path);
  }
  return meta_path.substr(0, meta_path.size() - suffix.size()) + ".qasm";
}

/**
 * @brief Execute a request JSON and serialize the response.
 *
 * This helper factors the common path shared by public entrypoints:
 * validation, backend dispatch, response generation, JSON serialization, and
 * mapping of response error codes to @ref qcore_status_t.
 *
 * @param in Request JSON (must contain `"qasm_path"` and must not contain `"qasm"`).
 * @param output_json Output pointer receiving an allocated, NUL-terminated JSON string.
 * @param output_len Output length in bytes excluding NUL terminator.
 * @param parse_ms Time spent parsing the request JSON.
 * @return Status code describing success or failure.
 *
 * @note The buffer returned in @p output_json must be freed with @ref qcore_free.
 */
static qcore_status_t run_in_json(const nlohmann::json& in,
                                 char** output_json,
                                 size_t* output_len,
                                 double parse_ms) {
  if (!output_json || !output_len) return QCORE_EINVALID;
  *output_json = nullptr;
  *output_len = 0;

  auto t0 = std::chrono::steady_clock::now();
  nlohmann::json out;

  // Validate / execute
  auto v = qcore::validate_fragment_meta(in);
  if (!v.ok) {
    out = qcore::make_error_result(in, "QCORE_EINVALID", v.message, v.details);
  } else {
    try {
      qcore::BackendInfo bi;
      qcore::BackendRunResult br = qcore::dispatch_run(in, bi);
      auto t1 = std::chrono::steady_clock::now();
      out = qcore::make_ok_result(in, bi, br, parse_ms, ms(t0, t1));
    } catch (const std::bad_alloc&) {
      out = qcore::make_error_result(in, "QCORE_EOOM", "Out of memory", nlohmann::json::object());
    } catch (const std::exception& e) {
      out = qcore::make_error_result(in, "QCORE_ERUNTIME", e.what(), nlohmann::json::object());
    }
  }

  // serialize response
  std::string s = out.dump(2);
  char* buf = (char*)std::malloc(s.size() + 1);
  if (!buf) return QCORE_EOOM;
  std::memcpy(buf, s.data(), s.size());
  buf[s.size()] = '\0';
  *output_json = buf;
  *output_len = s.size();

  // Map response to API status
  const std::string status = out.value("status", "error");
  if (status == "ok") return QCORE_OK;

  const std::string code = out.contains("error") ? out["error"].value("code", "") : "";
  if (code == "QCORE_EINVALID") return QCORE_EINVALID;
  if (code == "QCORE_EBACKEND") return QCORE_EBACKEND;
  if (code == "QCORE_EOOM") return QCORE_EOOM;
  return QCORE_ERUNTIME;
}

extern "C" {

/**
 * @brief Run a fragment given a meta JSON file, inferring the paired QASM file path.
 *
 * The function:
 * - Reads @p meta_json_path.
 * - Parses it as JSON.
 * - Infers the QASM path by strict suffix replacement (`.meta.json` -> `.qasm`).
 * - Attaches the inferred QASM path under `"qasm_path"` (no inline QASM text).
 * - Validates and dispatches the request and returns a JSON response.
 *
 * @param meta_json_path Path to `*.meta.json`.
 * @param output_json Output pointer receiving an allocated, NUL-terminated JSON string.
 * @param output_len Output length in bytes excluding NUL terminator.
 * @return Status code describing success or failure.
 *
 * @note The buffer returned in @p output_json must be freed with @ref qcore_free.
 */
qcore_status_t qcore_run_meta_file(const char* meta_json_path,
                                  char** output_json,
                                  size_t* output_len) {
  if (!meta_json_path || !output_json || !output_len) return QCORE_EINVALID;
  *output_json = nullptr;
  *output_len = 0;

  try {
    const std::string meta_path(meta_json_path);
    const auto meta_text = slurp_file_or_throw(meta_path);

    auto t_parse0 = std::chrono::steady_clock::now();
    nlohmann::json in = nlohmann::json::parse(meta_text);
    auto t_parse1 = std::chrono::steady_clock::now();
    const double parse_ms = ms(t_parse0, t_parse1);
    const std::string qasm_path = infer_qasm_path_from_meta_path_or_throw(meta_path);
    in["qasm_path"] = qasm_path;
    return run_in_json(in, output_json, output_len, parse_ms);

  } catch (const nlohmann::json::exception& e) {
    nlohmann::json empty = nlohmann::json::object();
    nlohmann::json out = qcore::make_error_result(empty, "QCORE_EINVALID",
                                                  std::string("Invalid JSON in meta file: ") + e.what(),
                                                  nlohmann::json::object());
    std::string s = out.dump(2);
    char* buf = (char*)std::malloc(s.size() + 1);
    if (!buf) return QCORE_EOOM;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *output_json = buf;
    *output_len = s.size();
    return QCORE_EINVALID;
  } catch (const std::bad_alloc&) {
    return QCORE_EOOM;
  } catch (const std::exception& e) {
    nlohmann::json empty = nlohmann::json::object();
    nlohmann::json details = nlohmann::json::object();
    details["meta_path"] = meta_json_path;
    nlohmann::json out = qcore::make_error_result(empty, "QCORE_ERUNTIME",
                                                  e.what(), details);
    std::string s = out.dump(2);
    char* buf = (char*)std::malloc(s.size() + 1);
    if (!buf) return QCORE_EOOM;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *output_json = buf;
    *output_len = s.size();
    return QCORE_ERUNTIME;
  }
}

/**
 * @brief Run a fragment given explicit meta and QASM file paths.
 *
 * This function reads: @p meta_json_path, parses it as JSON, and attaches
 * @p qasm_path under `"qasm_path"` (no inline QASM text). The request is then
 * validated and dispatched to the selected backend.
 *
 * @param meta_json_path Path to a meta JSON file.
 * @param qasm_path Path to the paired QASM file.
 * @param output_json Output pointer receiving an allocated, NUL-terminated JSON string.
 * @param output_len Output length in bytes excluding NUL terminator.
 * @return Status code describing success or failure.
 *
 * @note The buffer returned in @p output_json must be freed with @ref qcore_free.
 */
qcore_status_t qcore_run_paths(const char* meta_json_path,
                              const char* qasm_path,
                              char** output_json,
                              size_t* output_len) {
  if (!meta_json_path || !qasm_path || !output_json || !output_len) return QCORE_EINVALID;
  *output_json = nullptr;
  *output_len = 0;

  try {
    const auto meta_text = slurp_file_or_throw(std::string(meta_json_path));
    auto t_parse0 = std::chrono::steady_clock::now();
    nlohmann::json in = nlohmann::json::parse(meta_text);
    auto t_parse1 = std::chrono::steady_clock::now();
    const double parse_ms = ms(t_parse0, t_parse1);
    in["qasm_path"] = std::string(qasm_path);
    return run_in_json(in, output_json, output_len, parse_ms);

  } catch (const nlohmann::json::exception& e) {
    nlohmann::json empty = nlohmann::json::object();
    nlohmann::json out = qcore::make_error_result(empty, "QCORE_EINVALID",
                                                  std::string("Invalid JSON in meta file: ") + e.what(),
                                                  nlohmann::json::object());
    std::string s = out.dump(2);
    char* buf = (char*)std::malloc(s.size() + 1);
    if (!buf) return QCORE_EOOM;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *output_json = buf;
    *output_len = s.size();
    return QCORE_EINVALID;
  } catch (const std::bad_alloc&) {
    return QCORE_EOOM;
  } catch (const std::exception& e) {
    nlohmann::json empty = nlohmann::json::object();
    nlohmann::json details = nlohmann::json::object();
    details["meta_path"] = meta_json_path;
    details["qasm_path"] = qasm_path;
    nlohmann::json out = qcore::make_error_result(empty, "QCORE_ERUNTIME",
                                                  e.what(), details);
    std::string s = out.dump(2);
    char* buf = (char*)std::malloc(s.size() + 1);
    if (!buf) return QCORE_EOOM;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *output_json = buf;
    *output_len = s.size();
    return QCORE_ERUNTIME;
  }
}

/**
 * @brief Run a fragment given a meta JSON file path and an explicit routing hint.
 *
 * When route == 1 (QC), injects "backend_target":"qpu" and "_meta_file_path_"
 * into the request JSON so that dispatch_run() selects the QPU backend and
 * run_qpu_qulacs() can reference the original file path without re-writing it.
 */
qcore_status_t qcore_run_meta_file_ex(const char* meta_json_path,
                                      int route,
                                      char** output_json,
                                      size_t* output_len) {
  if (!meta_json_path || !output_json || !output_len) return QCORE_EINVALID;
  *output_json = nullptr;
  *output_len  = 0;

  try {
    const std::string meta_path(meta_json_path);
    const auto meta_text = slurp_file_or_throw(meta_path);

    auto t_parse0 = std::chrono::steady_clock::now();
    nlohmann::json in = nlohmann::json::parse(meta_text);
    auto t_parse1 = std::chrono::steady_clock::now();
    const double parse_ms = ms(t_parse0, t_parse1);

    const std::string qasm_path = infer_qasm_path_from_meta_path_or_throw(meta_path);
    in["qasm_path"] = qasm_path;

    // Inyectar backend_target según route
    // DMR_ROUTE_QC == 1  →  qpu
    // DMR_ROUTE_HPC == 0 →  cpu (comportamiento por defecto, no cambia nada)
    if (route == 1 /* DMR_ROUTE_QC */) {
      in["backend_target"]    = "qpu";
      in["_meta_file_path_"]  = meta_path;  // para que run_qpu_qulacs evite tmp file
    }

    return run_in_json(in, output_json, output_len, parse_ms);

  } catch (const nlohmann::json::exception& e) {
    nlohmann::json empty = nlohmann::json::object();
    nlohmann::json out = qcore::make_error_result(empty, "QCORE_EINVALID",
                           std::string("Invalid JSON in meta file: ") + e.what(),
                           nlohmann::json::object());
    std::string s = out.dump(2);
    char* buf = (char*)std::malloc(s.size() + 1);
    if (!buf) return QCORE_EOOM;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *output_json = buf;
    *output_len  = s.size();
    return QCORE_EINVALID;
  } catch (const std::bad_alloc&) {
    return QCORE_EOOM;
  } catch (const std::exception& e) {
    nlohmann::json empty = nlohmann::json::object();
    nlohmann::json details = nlohmann::json::object();
    details["meta_path"] = meta_json_path;
    nlohmann::json out = qcore::make_error_result(empty, "QCORE_ERUNTIME",
                           e.what(), details);
    std::string s = out.dump(2);
    char* buf = (char*)std::malloc(s.size() + 1);
    if (!buf) return QCORE_EOOM;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *output_json = buf;
    *output_len  = s.size();
    return QCORE_ERUNTIME;
  }
}


void qcore_free(void* p) { std::free(p); }
const char* qcore_version(void) { return "0.1.0"; }

} // extern "C"
