/**
 * @file qasm_mini_parser.cpp
 * @brief Minimal OpenQASM parser for extracting a linear QasmProgram.
 */
#include "qcore/qasm_mini_parser.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace qcore {

/** @brief Trim leading and trailing whitespace. */
static inline std::string trim(std::string s) {
  auto is_ws = [](unsigned char c){ return std::isspace(c)!=0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c){ return !is_ws(c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c){ return !is_ws(c); }).base(), s.end());
  return s;
}

/** @brief Return true if @p s starts with prefix @p p. */
static inline bool starts_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

/** @brief Strip single-line comments from a QASM line. */
static inline std::string strip_line_comment(std::string line) {
  auto p = line.find("//");
  if (p != std::string::npos) line = line.substr(0, p);
  p = line.find('#');
  if (p != std::string::npos) line = line.substr(0, p);
  return line;
}

/** @brief Parse an indexed token of the form name[idx] and return idx. */
static inline int parse_index(std::string_view tok) {
  auto l = tok.find('['), r = tok.find(']');
  if (l == std::string_view::npos || r == std::string_view::npos || r <= l + 1)
    throw std::runtime_error("Invalid indexed token: " + std::string(tok));
  return std::stoi(std::string(tok.substr(l+1, r-l-1)));
}

/** @brief Parse a limited angle expression into radians. */
static inline double parse_angle_expr(std::string expr) {
  // pi, -pi, pi/3, -pi/2, 2*pi, 2*pi/3, -2*pi/5, literals
  expr = trim(expr);
  expr.erase(std::remove(expr.begin(), expr.end(), ' '), expr.end());
  if (expr.empty()) throw std::runtime_error("Empty angle expression");

  double sign = 1.0;
  if (expr[0] == '+') expr.erase(expr.begin());
  else if (expr[0] == '-') { sign = -1.0; expr.erase(expr.begin()); }

  if (expr.find("pi") == std::string::npos && expr.find("PI") == std::string::npos)
    return sign * std::stod(expr);

  for (auto& c : expr) { if (c=='P') c='p'; if (c=='I') c='i'; }

  double coef = 1.0;
  std::string rest = expr;

  auto star = rest.find("*pi");
  if (star != std::string::npos) {
    coef = std::stod(rest.substr(0, star));
    rest = rest.substr(star + 1);
  }

  if (!starts_with(rest, "pi"))
    throw std::runtime_error("Unsupported angle: " + expr);

  rest = rest.substr(2);

  double denom = 1.0;
  if (!rest.empty()) {
    if (rest[0] == '/') denom = std::stod(rest.substr(1));
    else throw std::runtime_error("Unsupported angle suffix: " + expr);
  }

  return sign * coef * (M_PI / denom);
}

/** @brief Detect QASM dialect from source text. */
static inline QasmDialect detect_dialect(const std::string& qasm) {
  if (qasm.find("OPENQASM 3") != std::string::npos) return QasmDialect::QASM3;
  if (qasm.find("OPENQASM 2") != std::string::npos) return QasmDialect::QASM2;
  return QasmDialect::AUTO;
}

// Splitting / blocks

/** @brief Split QASM text into top-level statements, preserving brace blocks. */
static inline void split_statements_qasm3aware(const std::string& qasm,
                                               std::vector<std::string>& out) {
  out.clear();
  std::string cur;
  cur.reserve(256);

  int brace_depth = 0;

  for (size_t i = 0; i < qasm.size(); ) {
    size_t j = i;
    while (j < qasm.size() && qasm[j] != '\n') j++;
    std::string line = qasm.substr(i, j - i);
    line = strip_line_comment(line);
    line.push_back('\n');

    for (char ch : line) {
      if (ch == '{') {
        brace_depth++;
        cur.push_back(ch);
        continue;
      }
      if (ch == '}') {
        brace_depth--;
        if (brace_depth < 0) throw std::runtime_error("Unbalanced '}' in QASM");
        cur.push_back(ch);

        if (brace_depth == 0) {
          auto t = trim(cur);
          if (!t.empty()) out.push_back(t);
          cur.clear();
        }
        continue;
      }

      if (ch == ';' && brace_depth == 0) {
        auto t = trim(cur);
        if (!t.empty()) out.push_back(t);
        cur.clear();
        continue;
      }

      cur.push_back(ch);
    }

    i = (j < qasm.size()) ? (j + 1) : j;
  }

  auto t = trim(cur);
  if (!t.empty()) out.push_back(t);

  if (brace_depth != 0) throw std::runtime_error("Unbalanced '{' in QASM");
}

/** @brief Parse register declarations and update program sizes. */
static inline void parse_qreg_creg_or_qubit_bit(QasmProgram& p, const std::string& line) {
  // QASM2: qreg q[5]
  if (starts_with(line, "qreg")) {
    auto lb = line.find('['), rb = line.find(']');
    if (lb != std::string::npos && rb != std::string::npos)
      p.n_qubits = std::stoi(line.substr(lb+1, rb-lb-1));
    return;
  }
  if (starts_with(line, "creg")) {
    auto lb = line.find('['), rb = line.find(']');
    if (lb != std::string::npos && rb != std::string::npos)
      p.n_cbits = std::stoi(line.substr(lb+1, rb-lb-1));
    return;
  }

  // QASM3: qubit[5] q
  if (starts_with(line, "qubit[")) {
    auto lb = line.find('['), rb = line.find(']');
    if (lb != std::string::npos && rb != std::string::npos)
      p.n_qubits = std::stoi(line.substr(lb+1, rb-lb-1));
    return;
  }
  // QASM3: bit[5] c
  if (starts_with(line, "bit[")) {
    auto lb = line.find('['), rb = line.find(']');
    if (lb != std::string::npos && rb != std::string::npos)
      p.n_cbits = std::stoi(line.substr(lb+1, rb-lb-1));
    return;
  }
}

/** @brief Extract and return the contents of a brace-delimited block starting at @p pos. */
static inline std::string extract_brace_block(const std::string& s, size_t& pos) {
  if (pos >= s.size() || s[pos] != '{') throw std::runtime_error("Expected '{'");
  int depth = 0;
  size_t start = pos;
  for (; pos < s.size(); ++pos) {
    if (s[pos] == '{') depth++;
    else if (s[pos] == '}') {
      depth--;
      if (depth == 0) {
        std::string inner = s.substr(start + 1, pos - start - 1);
        pos++; // past '}'
        return inner;
      }
    }
  }
  throw std::runtime_error("Unbalanced braces");
}

/** @brief Parse a block body into a list of operations. */
static inline std::vector<QasmOp> parse_block_ops(const std::string& body, QasmDialect d) {
  QasmProgram tmp = parse_qasm_mini(body, d);
  return tmp.ops;
}

/** @brief Parse U gate parameters (theta, phi, lambda) in radians. */
static inline void parse_u_angles(std::string inside, double& th, double& ph, double& la) {
  // inside: "theta,phi,lambda"
  inside = trim(inside);
  inside.erase(std::remove(inside.begin(), inside.end(), ' '), inside.end());

  std::vector<std::string> parts;
  std::string cur;
  for (char ch : inside) {
    if (ch == ',') { parts.push_back(cur); cur.clear(); }
    else cur.push_back(ch);
  }
  if (!cur.empty()) parts.push_back(cur);

  if (parts.size() != 3)
    throw std::runtime_error("U/u expects 3 params: U(theta,phi,lambda)");

  th = parse_angle_expr(parts[0]);
  ph = parse_angle_expr(parts[1]);
  la = parse_angle_expr(parts[2]);
}

/** @brief Parse U gate parameters (theta, phi, lambda) in radians. */
QasmProgram parse_qasm_mini(const std::string& qasm, QasmDialect dialect) {
  QasmProgram p;
  p.dialect = (dialect == QasmDialect::AUTO) ? detect_dialect(qasm) : dialect;

  std::vector<std::string> stmts;
  split_statements_qasm3aware(qasm, stmts);

  for (const auto& raw : stmts) {
    std::string line = trim(raw);
    if (line.empty()) continue;

    // headers/includes
    if (starts_with(line, "OPENQASM")) continue;
    if (starts_with(line, "include")) continue;
    if (starts_with(line, "barrier")) continue;

    // decls
    parse_qreg_creg_or_qubit_bit(p, line);
    if (starts_with(line, "qreg") || starts_with(line, "creg") ||
        starts_with(line, "qubit[") || starts_with(line, "bit[")) {
      continue;
    }

    // --- measure QASM2: measure q[i] -> c[j]
    if (starts_with(line, "measure")) {
      auto sp = line.find(' ');
      if (sp == std::string::npos) throw std::runtime_error("Invalid measure: " + line);
      auto rest = trim(line.substr(sp + 1));
      auto arrow = rest.find("->");
      if (arrow == std::string::npos) throw std::runtime_error("Invalid measure arrow: " + line);
      int qidx = parse_index(trim(rest.substr(0, arrow)));
      int cidx = parse_index(trim(rest.substr(arrow + 2)));
      QasmOp op; op.kind = OpKind::MEASURE; op.q0 = qidx; op.c0 = cidx;
      p.ops.push_back(op);
      continue;
    }

    // --- measure QASM3: c[i] = measure q[j]
    if (line.find("= measure") != std::string::npos) {
      auto eq = line.find('=');
      auto lhs = trim(line.substr(0, eq));
      auto rhs = trim(line.substr(eq + 1)); // "measure q[0]"
      auto sp = rhs.find(' ');
      if (sp == std::string::npos) throw std::runtime_error("Invalid qasm3 measure: " + line);
      auto qtok = trim(rhs.substr(sp + 1));
      int cidx = parse_index(lhs);
      int qidx = parse_index(qtok);
      QasmOp op; op.kind = OpKind::MEASURE; op.q0 = qidx; op.c0 = cidx;
      p.ops.push_back(op);
      continue;
    }

    if (starts_with(line, "if")) {
    auto lpar = line.find('('), rpar = line.find(')');
    if (lpar == std::string::npos || rpar == std::string::npos || rpar <= lpar + 1)
      throw std::runtime_error("Invalid if syntax: " + line);

    std::string cond = trim(line.substr(lpar + 1, rpar - lpar - 1));
    cond.erase(std::remove(cond.begin(), cond.end(), ' '), cond.end());

    bool neg = false;
    if (!cond.empty() && cond[0] == '!') { neg = true; cond.erase(cond.begin()); }

    int cidx = parse_index(cond); // c[i]

    size_t pos = line.find('{', rpar);
    if (pos == std::string::npos) throw std::runtime_error("Missing '{' in if: " + line);
    std::string then_body = extract_brace_block(line, pos);

    std::string else_body;
    auto else_pos = line.find("else", pos);
    if (else_pos != std::string::npos) {
      size_t bpos = line.find('{', else_pos);
      if (bpos == std::string::npos) throw std::runtime_error("Missing '{' in else: " + line);
      else_body = extract_brace_block(line, bpos);
    }

    QasmOp op;
    op.kind = OpKind::IF;
    op.c0 = cidx;
    op.cond_negated = neg;
    op.then_ops = parse_block_ops(then_body, p.dialect);
    if (!else_body.empty()) op.else_ops = parse_block_ops(else_body, p.dialect);

    p.ops.push_back(std::move(op));
    continue;
  }

    // --- helpers gates 1q without params
    auto parse_1q = [&](std::string_view op, OpKind k)->bool{
      if (!starts_with(line, op)) return false;
      auto sp = line.find(' ');
      if (sp == std::string::npos) throw std::runtime_error("Invalid 1q: " + line);
      int qidx = parse_index(std::string_view(line).substr(sp + 1));
      QasmOp opn; opn.kind = k; opn.q0 = qidx;
      p.ops.push_back(opn);
      return true;
    };
    if (parse_1q("id", OpKind::I)) continue;
    if (parse_1q("x",  OpKind::X)) continue;
    if (parse_1q("y",  OpKind::Y)) continue;
    if (parse_1q("z",  OpKind::Z)) continue;
    if (parse_1q("h",  OpKind::H)) continue;
    if (parse_1q("s",  OpKind::S)) continue;
    if (parse_1q("sdg",OpKind::SDG)) continue;
    if (parse_1q("t",  OpKind::T)) continue;
    if (parse_1q("tdg",OpKind::TDG)) continue;

    // --- rotations rx/ry/rz(theta) q[i]
    auto parse_rot = [&](std::string_view op, OpKind k)->bool{
      if (!starts_with(line, op)) return false;
      auto lpar = line.find('('), rpar = line.find(')');
      if (lpar == std::string::npos || rpar == std::string::npos || rpar <= lpar + 1)
        throw std::runtime_error("Invalid rot: " + line);
      double th = parse_angle_expr(line.substr(lpar + 1, rpar - lpar - 1));
      auto sp = line.find(' ', rpar);
      if (sp == std::string::npos) throw std::runtime_error("Invalid rot target: " + line);
      int qidx = parse_index(std::string_view(line).substr(sp + 1));
      QasmOp opn; opn.kind = k; opn.q0 = qidx; opn.theta = th;
      p.ops.push_back(opn);
      return true;
    };
    if (parse_rot("rx", OpKind::RX)) continue;
    if (parse_rot("ry", OpKind::RY)) continue;
    if (parse_rot("rz", OpKind::RZ)) continue;

    // --- U/u(theta,phi,lambda) q[i]  (QASM3 stdgates: U; QASM2: u3/u)
    if (starts_with(line, "U") || starts_with(line, "u") || starts_with(line, "u3")) {
      auto lpar = line.find('('), rpar = line.find(')');
      if (lpar == std::string::npos || rpar == std::string::npos || rpar <= lpar + 1)
        throw std::runtime_error("Invalid U/u: " + line);

      double th=0, ph=0, la=0;
      parse_u_angles(line.substr(lpar + 1, rpar - lpar - 1), th, ph, la);

      auto sp = line.find(' ', rpar);
      if (sp == std::string::npos) throw std::runtime_error("Invalid U/u target: " + line);
      int qidx = parse_index(std::string_view(line).substr(sp + 1));

      QasmOp opn; opn.kind = OpKind::U; opn.q0 = qidx;
      opn.theta = th; opn.phi = ph; opn.lambda = la;
      p.ops.push_back(opn);
      continue;
    }

    // --- CX/CZ/SWAP
    auto parse_2q = [&](std::string_view op, OpKind k)->bool{
      if (!starts_with(line, op)) return false;
      auto sp = line.find(' ');
      if (sp == std::string::npos) throw std::runtime_error("Invalid 2q: " + line);
      auto args = trim(line.substr(sp + 1));
      auto comma = args.find(',');
      if (comma == std::string::npos) throw std::runtime_error("Invalid 2q args: " + line);
      int q0 = parse_index(trim(args.substr(0, comma)));
      int q1 = parse_index(trim(args.substr(comma + 1)));
      QasmOp opn; opn.kind = k; opn.q0 = q0; opn.q1 = q1;
      p.ops.push_back(opn);
      return true;
    };
    if (parse_2q("cx",   OpKind::CX)) continue;
    if (parse_2q("CX",   OpKind::CX)) continue;
    if (parse_2q("cz",   OpKind::CZ)) continue;
    if (parse_2q("CZ",   OpKind::CZ)) continue;
    if (parse_2q("swap", OpKind::SWAP)) continue;
    if (parse_2q("SWAP", OpKind::SWAP)) continue;

    throw std::runtime_error("Unsupported QASM statement (mini parser): " + line);
  }

  return p;
}

} // namespace qcore
