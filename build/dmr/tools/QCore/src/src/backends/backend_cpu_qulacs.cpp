/**
 * @file backend_cpu_qulacs.cpp
 * @brief CPU backend implementation using Qulacs.
 *
 * Provides execution of OpenQASM circuits on the CPU via the Qulacs simulator.
 * The backend supports two execution paths:
 * - Direct circuit construction for OpenQASM 2.
 * - A minimal interpreter for a supported subset of OpenQASM 3, including
 *   unitary gates, measurement, and simple conditional control flow.
 *
 * The backend computes expectation values in the Z basis and reports
 * timing information for simulation and observable evaluation.
 */
#include "backends/backend_common.h"
#include "qcore/qasm_mini_parser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fstream>
#include <sstream>

#if QCORE_HAS_QULACS
  #include <cppsim/circuit.hpp>
  #include <cppsim/state.hpp>
  #include <cppsim/gate_factory.hpp>
#endif

namespace qcore {

/** @brief Trim leading and trailing whitespace. */
static inline std::string trim(std::string s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c){ return !is_ws(c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c){ return !is_ws(c); }).base(), s.end());
  return s;
}

/** @brief Return true if @p s starts with prefix @p p. */
static inline bool starts_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

/** @brief Split QASM text into statements using ';' as delimiter. */
static inline void split_semicolon(const std::string& qasm, std::vector<std::string>& out_cmds) {
  out_cmds.clear();
  std::string cur;
  cur.reserve(128);
  for (char ch : qasm) {
    if (ch == ';') {
      auto t = trim(cur);
      if (!t.empty()) out_cmds.push_back(t);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  auto t = trim(cur);
  if (!t.empty()) out_cmds.push_back(t);
}

/** @brief Parse a qubit token of the form name[idx] and return idx. */
static inline int parse_q_index(std::string_view token) {
  auto l = token.find('[');
  auto r = token.find(']');
  if (l == std::string_view::npos || r == std::string_view::npos || r <= l + 1) {
    throw std::runtime_error("Invalid qubit token: " + std::string(token));
  }
  return std::stoi(std::string(token.substr(l + 1, r - l - 1)));
}

/** @brief Parse a limited angle expression into radians. */
static inline double parse_angle_expr(std::string expr) {
  expr = trim(expr);
  expr.erase(std::remove(expr.begin(), expr.end(), ' '), expr.end());
  if (expr.empty()) throw std::runtime_error("Empty angle expression");

  double sign = 1.0;
  if (expr[0] == '+') expr.erase(expr.begin());
  else if (expr[0] == '-') { sign = -1.0; expr.erase(expr.begin()); }

  if (expr.find("pi") == std::string::npos && expr.find("PI") == std::string::npos) {
    return sign * std::stod(expr);
  }

  for (auto& c : expr) {
    if (c == 'P') c = 'p';
    if (c == 'I') c = 'i';
  }

  double coef = 1.0;
  std::string rest = expr;

  auto star = rest.find("*pi");
  if (star != std::string::npos) {
    coef = std::stod(rest.substr(0, star));
    rest = rest.substr(star + 1);
  }

  if (!starts_with(rest, "pi")) {
    throw std::runtime_error("Unsupported angle expression: " + expr);
  }
  rest = rest.substr(2);

  double denom = 1.0;
  if (!rest.empty()) {
    if (rest[0] == '/') denom = std::stod(rest.substr(1));
    else throw std::runtime_error("Unsupported angle expression suffix: " + expr);
  }

  return sign * coef * (M_PI / denom);
}

#if QCORE_HAS_QULACS
/** @brief Parse a limited angle expression into radians. */
static inline double prob_one_on_qubit(const QuantumState& st, int q) {
  const auto dim = (std::uint64_t)st.dim;
  const auto* psi = st.data_cpp();
  const std::uint64_t mask = (1ULL << (std::uint64_t)q);
  double p1 = 0.0;
  for (std::uint64_t i = 0; i < dim; ++i) {
    if (i & mask) p1 += std::norm(psi[i]);
  }
  if (p1 < 0) p1 = 0;
  if (p1 > 1) p1 = 1;
  return p1;
}

/** @brief Project qubit @p q onto @p outcome and renormalize state in place. */
static inline void project_qubit_inplace(QuantumState& st, int q, int outcome) {
  const auto dim = (std::uint64_t)st.dim;
  auto* psi = st.data_cpp();
  const std::uint64_t mask = (1ULL << (std::uint64_t)q);

  double norm2 = 0.0;
  for (std::uint64_t i = 0; i < dim; ++i) {
    const int bit = (i & mask) ? 1 : 0;
    if (bit != outcome) {
      psi[i] = 0.0;
    } else {
      norm2 += std::norm(psi[i]);
    }
  }
  const double inv = (norm2 > 0) ? (1.0 / std::sqrt(norm2)) : 0.0;
  for (std::uint64_t i = 0; i < dim; ++i) psi[i] *= inv;
}

/** @brief Apply a supported unitary mini-op to a Qulacs state. */
static inline void apply_mini_op_unitary(QuantumState& st, const QasmOp& op) {
  switch (op.kind) {
    case OpKind::H:  gate::H((UINT)op.q0)->update_quantum_state(&st); break;
    case OpKind::X:  gate::X((UINT)op.q0)->update_quantum_state(&st); break;
    case OpKind::Z:  gate::Z((UINT)op.q0)->update_quantum_state(&st); break;
    case OpKind::RX: gate::RX((UINT)op.q0, op.theta)->update_quantum_state(&st); break;
    case OpKind::RY: gate::RY((UINT)op.q0, op.theta)->update_quantum_state(&st); break;
    case OpKind::RZ: gate::RZ((UINT)op.q0, op.theta)->update_quantum_state(&st); break;
    case OpKind::CZ: gate::CZ((UINT)op.q0, (UINT)op.q1)->update_quantum_state(&st); break;
    default:
      throw std::runtime_error("apply_mini_op_unitary: non-unitary or unsupported op");
  }
}

/** @brief Execute a sequence of mini-ops on a state, updating classical bits. */
static inline void exec_ops_qulacs_mini(const std::vector<QasmOp>& ops,
                                       QuantumState& st,
                                       std::vector<uint8_t>& cbits,
                                       std::mt19937_64& rng) {
  std::uniform_real_distribution<double> uni(0.0, 1.0);

  for (const auto& op : ops) {
    if (op.kind == OpKind::MEASURE) {
      const int q = op.q0;
      const int c = op.c0;
      const double p1 = prob_one_on_qubit(st, q);
      const double u = uni(rng);
      const int outcome = (u < p1) ? 1 : 0;
      project_qubit_inplace(st, q, outcome);
      if (c >= 0 && c < (int)cbits.size()) cbits[(size_t)c] = (uint8_t)outcome;
      continue;
    }

    if (op.kind == OpKind::IF) {
      const int c = op.c0;
      const int bit = (c >= 0 && c < (int)cbits.size()) ? (int)cbits[(size_t)c] : 0;
      const bool take_then = op.cond_negated ? (bit == 0) : (bit != 0);
      const auto& branch = take_then ? op.then_ops : op.else_ops;
      exec_ops_qulacs_mini(branch, st, cbits, rng);
      continue;
    }

    apply_mini_op_unitary(st, op);
  }
}

/** @brief Estimate Z-basis expectation value by shot-based trajectories. */
static inline double run_program_expected_value_trajectories_qulacs(const QasmProgram& prog,
                                                                    int shots,
                                                                    std::uint64_t z_mask) {
  if (prog.n_qubits <= 0) return 0.0;

  std::random_device rd;
  std::mt19937_64 rng(((std::uint64_t)rd() << 32) ^ (std::uint64_t)rd());

  long long acc = 0;

  for (int s = 0; s < shots; ++s) {
    QuantumState st(prog.n_qubits);
    st.set_zero_state();
    std::vector<uint8_t> cbits((size_t)std::max(1, prog.n_cbits), 0);

    exec_ops_qulacs_mini(prog.ops, st, cbits, rng);

    auto one = st.sampling(1);
    const std::uint64_t sample = one.empty() ? 0ULL : (std::uint64_t)one[0];
    const int parity = __builtin_popcountll(sample & z_mask) & 1;
    acc += parity ? -1 : +1;
  }

  return shots > 0 ? (double)acc / (double)shots : 0.0;
}

/** @brief Build a Qulacs circuit from a limited OpenQASM 2 subset. */
static inline std::unique_ptr<QuantumCircuit>
build_qulacs_circuit_from_qasm2(const std::string& qasm, int n_qubits) {
  auto circuit = std::make_unique<QuantumCircuit>(n_qubits);

  std::vector<std::string> cmds;
  split_semicolon(qasm, cmds);

  for (const auto& raw : cmds) {
    const std::string line = trim(raw);
    if (line.empty()) continue;

    // Ignore headers / declarations
    if (starts_with(line, "OPENQASM")) continue;
    if (starts_with(line, "include")) continue;
    if (starts_with(line, "qreg")) continue;
    if (starts_with(line, "creg")) continue;
    if (starts_with(line, "barrier")) continue;
    if (starts_with(line, "measure")) continue;

    // h
    if (starts_with(line, "h")) {
      auto sp = line.find(' ');
      if (sp == std::string::npos) throw std::runtime_error("Invalid h syntax: " + line);
      int q = parse_q_index(line.substr(sp + 1));
      circuit->add_gate(gate::H((UINT)q));
      continue;
    }

    // x
    if (starts_with(line, "x")) {
      auto sp = line.find(' ');
      if (sp == std::string::npos) throw std::runtime_error("Invalid x syntax: " + line);
      int q = parse_q_index(line.substr(sp + 1));
      circuit->add_gate(gate::X((UINT)q));
      continue;
    }

    // z
    if (starts_with(line, "z")) {
      auto sp = line.find(' ');
      if (sp == std::string::npos) throw std::runtime_error("Invalid z syntax: " + line);
      int q = parse_q_index(line.substr(sp + 1));
      circuit->add_gate(gate::Z((UINT)q));
      continue;
    }

    // cz q[i], q[j]
    if (starts_with(line, "cz")) {
      auto sp = line.find(' ');
      if (sp == std::string::npos) throw std::runtime_error("Invalid cz syntax: " + line);
      std::string args = trim(line.substr(sp + 1));
      auto comma = args.find(',');
      if (comma == std::string::npos) throw std::runtime_error("Invalid cz args: " + line);
      int q0 = parse_q_index(trim(args.substr(0, comma)));
      int q1 = parse_q_index(trim(args.substr(comma + 1)));
      circuit->add_gate(gate::CZ((UINT)q0, (UINT)q1));
      continue;
    }

    auto parse_rot = [&](const char* opname, auto gate_factory) {
      const std::string op(opname);
      if (!starts_with(line, op)) return false;
      auto lpar = line.find('(');
      auto rpar = line.find(')');
      if (lpar == std::string::npos || rpar == std::string::npos || rpar <= lpar + 1)
        throw std::runtime_error("Invalid rotation syntax: " + line);
      std::string angle_expr = line.substr(lpar + 1, rpar - lpar - 1);
      double theta = parse_angle_expr(angle_expr);
      auto sp = line.find(' ', rpar);
      if (sp == std::string::npos) throw std::runtime_error("Invalid rotation target: " + line);
      int q = parse_q_index(line.substr(sp + 1));
      circuit->add_gate(gate_factory((UINT)q, theta));
      return true;
    };

    if (parse_rot("rx", [](UINT q, double t) { return gate::RX(q, t); })) continue;
    if (parse_rot("ry", [](UINT q, double t) { return gate::RY(q, t); })) continue;
    if (parse_rot("rz", [](UINT q, double t) { return gate::RZ(q, t); })) continue;

    throw std::runtime_error("Unsupported QASM2 statement (mini parser): " + line);
  }

  return circuit;
}

/** @brief Estimate expectation value from computational basis samples. */
template <class SampleT>
static inline double estimate_expectation_from_samples(const std::vector<SampleT>& samples,
                                                       std::uint64_t z_mask) {
  static_assert(std::is_integral<SampleT>::value, "SampleT must be integral");
  long long acc = 0;
  for (auto s : samples) {
    std::uint64_t x = (std::uint64_t)s & z_mask;
    const int parity = __builtin_popcountll(x) & 1;
    acc += parity ? -1 : 1;
  }
  return samples.empty() ? 0.0 : (double)acc / (double)samples.size();
}
#endif // QCORE_HAS_QULACS

/** @brief Return default shots from environment or fallback value. */
static inline int default_shots() {
  const char* env = std::getenv("QCORE_SHOTS");
  if (!env) return 1024;
  try {
    int s = std::stoi(env);
    return (s > 0) ? s : 1024;
  } catch (...) {
    return 1024;
  }
}

/** @brief Compute sign for a gate-cut term index. digits base-6 in {3,5}. */
static inline int gate_cut_term_sign(int index) {
  int number = index;
  bool change_sign = false;
  while (number != 0) {
    int digit = number % 6;
    if (digit == 3 || digit == 5) change_sign = !change_sign;
    number /= 6;
  }
  return change_sign ? -1 : +1;
}

/** @brief Build Pauli string and Z-mask from meta observable fields. */
static inline void build_pauli_and_mask_from_meta(const nlohmann::json& meta,
                                                  int n_qubits,
                                                  std::string& out_pauli,
                                                  std::uint64_t& out_z_mask) {
  out_pauli.assign((size_t)n_qubits, 'Z');
  std::vector<int> obs_i;
  if (meta.contains("obs_i_qubits") && meta["obs_i_qubits"].is_array()) {
    for (const auto& v : meta["obs_i_qubits"]) {
      if (!v.is_number_integer()) continue;
      int idx = v.get<int>();
      if (0 <= idx && idx < n_qubits) obs_i.push_back(idx);
    }
  }
  for (int idx : obs_i) out_pauli[(size_t)idx] = 'I';

  out_z_mask = 0;
  for (int q = 0; q < n_qubits; ++q) {
    if (out_pauli[(size_t)q] == 'Z') out_z_mask |= (std::uint64_t(1) << q);
  }
}

/** @brief Read entire file contents into a string. */
static std::string slurp_file_or_throw(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot open QASM file: " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/** @brief Load QASM source text from meta-provided file path. */
static std::string resolve_qasm_text_or_throw(const nlohmann::json& meta) {
  if (!meta.contains("qasm_path") || !meta["qasm_path"].is_string()) {
    throw std::runtime_error("Missing required field: qasm_path");
  }
  const std::string p = meta["qasm_path"].get<std::string>();
  if (p.empty()) throw std::runtime_error("qasm_path is empty");
  return slurp_file_or_throw(p);
}

/** @brief Execute a fragment on CPU using Qulacs. */
BackendRunResult run_cpu_qulacs(const nlohmann::json& meta, BackendInfo& info) {
  (void)info;

  BackendRunResult r;

#if !QCORE_HAS_QULACS
  throw std::runtime_error("Qulacs backend not available (QCORE_HAS_QULACS=0).");
#else
  const std::string qasm = resolve_qasm_text_or_throw(meta);
  const int n = meta.at("n_qubits").get<int>();
  const int qv = meta.value("qasm_version", 2);

  const bool is_gate_cut = meta.value("gate_cut", false);
  r.cut_type = is_gate_cut ? CutType::Gate : CutType::Wire;

  if (is_gate_cut) {
    int idx = -1;
    if (meta.contains("gate_cut_index") && meta["gate_cut_index"].is_number_integer()) idx = meta["gate_cut_index"].get<int>();
    else if (meta.contains("term_index") && meta["term_index"].is_number_integer()) idx = meta["term_index"].get<int>();
    else if (meta.contains("index") && meta["index"].is_number_integer()) idx = meta["index"].get<int>();
    if (idx >= 0) { r.gate_cut_index = idx; r.gate_cut_sign = gate_cut_term_sign(idx); }
  }

  const int shots = default_shots();
  r.shots = shots;

  std::uint64_t z_mask = 0;
  build_pauli_and_mask_from_meta(meta, n, r.pauli, z_mask);

  auto t_sim0 = std::chrono::steady_clock::now();

  if (qv == 2) {
    auto circuit = build_qulacs_circuit_from_qasm2(qasm, n);
    QuantumState state(n);
    state.set_zero_state();
    circuit->update_quantum_state(&state);

    auto t_sim1 = std::chrono::steady_clock::now();
    r.simulate_ms = std::chrono::duration<double, std::milli>(t_sim1 - t_sim0).count();

    auto t_ev0 = std::chrono::steady_clock::now();
    auto samples = state.sampling(shots);
    r.expected_value = estimate_expectation_from_samples(samples, z_mask);
    auto t_ev1 = std::chrono::steady_clock::now();
    r.observables_ms = std::chrono::duration<double, std::milli>(t_ev1 - t_ev0).count();

    return r;
  }

  // QASM3 mini parser path: supports H/X/Z/Rx/Ry/Rz/CZ + MEASURE + if(!bit){x}
  QasmProgram prog = parse_qasm_mini(qasm, QasmDialect::QASM3);
  if (prog.n_qubits <= 0) prog.n_qubits = n;
  if (prog.n_cbits  <= 0) prog.n_cbits  = meta.value("n_cbits", std::max(1, n));

  auto t_sim1 = std::chrono::steady_clock::now();
  r.simulate_ms = std::chrono::duration<double, std::milli>(t_sim1 - t_sim0).count();

  auto t_ev0 = std::chrono::steady_clock::now();
  r.expected_value = run_program_expected_value_trajectories_qulacs(prog, shots, z_mask);
  auto t_ev1 = std::chrono::steady_clock::now();
  r.observables_ms = std::chrono::duration<double, std::milli>(t_ev1 - t_ev0).count();

  return r;
#endif
}

} // namespace qcore
