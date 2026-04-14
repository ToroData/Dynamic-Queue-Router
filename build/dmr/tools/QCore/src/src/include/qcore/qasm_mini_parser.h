/**
 * @file qasm_mini_parser.h
 * @brief Minimal OpenQASM parsing interface.
 */
#pragma once
#include <string>
#include <vector>

namespace qcore {

/**
 * @brief Supported OpenQASM dialects.
 */
enum class QasmDialect { QASM2, QASM3, AUTO };

/**
 * @brief Operation kinds supported by the mini QASM parser.
 */
enum class OpKind {
  I, X, Y, Z, H, S, SDG, T, TDG,
  RX, RY, RZ,
  U,
  P,
  CX,
  CZ,
  SWAP,
  MEASURE,
  IF
};

/**
 * @brief Single QASM operation.
 */
struct QasmOp {
  OpKind kind{};
  int q0{-1};
  int q1{-1};
  int c0{-1};
  double theta{0.0};
  double phi{0.0};
  double lambda{0.0};
  bool cond_negated{false};
  std::vector<QasmOp> then_ops;
  std::vector<QasmOp> else_ops;
};

/**
 * @brief Parsed QASM program representation.
 */
struct QasmProgram {
  QasmDialect dialect{QasmDialect::AUTO};
  int n_qubits{0};
  int n_cbits{0};
  std::vector<QasmOp> ops;
};

/**
 * @brief Parse OpenQASM source into a QasmProgram.
 */
QasmProgram parse_qasm_mini(const std::string& qasm,
                            QasmDialect dialect = QasmDialect::AUTO);

} // namespace qcore
