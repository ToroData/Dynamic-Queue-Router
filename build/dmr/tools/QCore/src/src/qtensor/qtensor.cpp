/**
 * @file qtensor_reconstruct.cpp
 * @brief QTensor reconstruction utilities.
 */

#include "qtensor/qtensor.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace qtensor {
namespace fs = std::filesystem;

/** @brief Compute b^e for e >= 0. */
static std::uint64_t pow_u64(std::uint64_t b, int e) {
  if (e < 0) throw std::invalid_argument("number_cuts (k) must be >= 0");
  std::uint64_t r = 1;
  for (int i = 0; i < e; i++) r *= b;
  return r;
}

/** @brief Compute sign for gate cuts from base-6 digit parity. */
static inline int sign_from_digits_gate6(std::uint64_t j, int k) {
  bool flip = false;
  for (int i = 0; i < k; i++) {
    std::uint64_t d = j % 6;
    if (d == 3 || d == 5) flip = !flip;
    j /= 6;
  }
  return flip ? -1 : +1;
}

/** @brief Compute sign for wire cuts from base-8 digit parity {3,5,7}. */
static inline int sign_from_digits_wire8(std::uint64_t j, int k) {
  bool flip = false;
  for (int i = 0; i < k; i++) {
    std::uint64_t d = j % 8;
    if (d == 3 || d == 5 || d == 7) flip = !flip;
    j /= 8;
  }
  return flip ? -1 : +1;
}

/** @brief Reconstruct scalar value from expectation values and cut metadata. */
ReconstructResult reconstruct(const std::vector<double>& ev,
                              CutType cut_type,
                              int number_cuts,
                              std::int64_t number_components) {
  if (number_cuts < 0) throw std::invalid_argument("number_cuts must be >= 0");
  const std::uint64_t n = static_cast<std::uint64_t>(ev.size());
  if (n == 0) throw std::invalid_argument("ev must be non-empty");

  const std::uint64_t B = (cut_type == CutType::GATE) ? 6ull : 8ull;
  const std::uint64_t terms = pow_u64(B, number_cuts);

  if (terms == 0) throw std::runtime_error("terms overflow");
  if (n % terms != 0) {
    throw std::invalid_argument("ev.size() is not divisible by B^k. "
                                "Inconsistent (n, cut_type, k).");
  }

  bool inferred = false;
  std::uint64_t m = 0;

  if (number_components < 0) {
    m = n / terms;
    inferred = true;
  } else {
    m = static_cast<std::uint64_t>(number_components);
    if (m == 0) throw std::invalid_argument("number_components (m) must be > 0");
    if (m * terms != n) {
      throw std::invalid_argument("Inconsistent: m * (B^k) != ev.size()");
    }
  }

  // Accumulate sum_j s(j)*prod(ev_block)
  double acc = 0.0;

  // TODO (04/02/2026): vectorize
  for (std::uint64_t j = 0; j < terms; j++) {
    const std::uint64_t base_idx = j * m;

    double prod = 1.0;
    for (std::uint64_t c = 0; c < m; c++) {
      prod *= ev[base_idx + c];
    }

    const int s = (cut_type == CutType::GATE)
                    ? sign_from_digits_gate6(j, number_cuts)
                    : sign_from_digits_wire8(j, number_cuts);

    acc += static_cast<double>(s) * prod;
  }
  const double denom = std::ldexp(1.0, number_cuts);
  const double R = acc / denom;

  ReconstructResult out;
  out.R = R;
  out.base = B;
  out.terms = terms;
  out.m = m;
  out.n = n;
  out.m_inferred = inferred;
  return out;
}

/** @brief Convert cut type enum to string. */
static std::string cut_type_to_string(CutType t) {
  return (t == CutType::GATE) ? "GATE" : "WIRE";
}

/** @brief Build reconstruction JSON output from expectation values and options. */
nlohmann::json reconstruct_json(const std::vector<double>& ev,
                                const ReconstructOptions& opt) {
  if (opt.output_dir.empty())
    throw std::invalid_argument("output_dir must be provided");

  const auto res = reconstruct(ev, opt.cut_type, opt.number_cuts, opt.number_components);

  fs::create_directories(opt.output_dir);
  const fs::path outpath = fs::path(opt.output_dir) / opt.output_filename;

  nlohmann::json j;
  j["schema"] = "qtensor.reconstruction.v1";
  j["job_id"] = opt.job_id.empty() ? nullptr : nlohmann::json(opt.job_id);
  j["fragment_id"] = opt.fragment_id.empty() ? nullptr : nlohmann::json(opt.fragment_id);

  j["cut_type"] = cut_type_to_string(opt.cut_type);
  j["number_cuts"] = opt.number_cuts;
  j["base"] = res.base;
  j["terms"] = res.terms;
  j["number_components"] = res.m;
  j["m_inferred"] = res.m_inferred;

  j["n_ev"] = res.n;
  j["global_factor"] = 1.0 / std::ldexp(1.0, opt.number_cuts); // 1/2^k
  j["result_R"] = res.R;

  j["ev_ordering"] = "blocks: j-major; ev[j*m + c]";

  return j;
}

} // namespace qtensor
