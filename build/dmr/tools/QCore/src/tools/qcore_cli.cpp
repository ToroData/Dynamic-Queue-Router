/**
 * @file qcore_cli.cpp
 * @brief CLI for QCore (DMR–DQR–QCut invocation).
 *
 * This CLI supports only the production invocation pattern:
 *   - qcore_cli <frag.meta.json>
 *
 * It loads the fragment metadata from file and infers the paired QASM path
 * by strict suffix replacement:
 *   frag_000.meta.json  ->  frag_000.qasm
 *
 * The CLI calls the QCore C API:
 *   - qcore_run_meta_file() for the inferred QASM mode.
 *
 * Output:
 *   - Writes the JSON response to stdout.
 *   - Returns qcore_status_t as process exit code.
 */

#include "qcore/qcore.h"

#include <iostream>
#include <string>

/**
 * @brief Check if a string ends with the given suffix.
 * @param s Input string.
 * @param suf Suffix.
 * @return True if @p s ends with @p suf.
 */
static bool ends_with(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

/**
 * @brief Print CLI usage information to stderr.
 * @param prog Program name (argv[0]).
 */
static void usage(const char* prog) {
  std::cerr
    << "Usage:\n"
    << "  " << prog << " <frag.meta.json>\n";
}

/**
 * @brief Entry point for the production QCore CLI.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Process exit code (cast from qcore_status_t), or 2/3 for CLI errors.
 *
 * @details
 * Accepted form:
 *  - argc==2 and argv[1] ends with ".meta.json"
 *      Calls qcore_run_meta_file(meta_path, &out, &out_len).
 *
 */
int main(int argc, char** argv) {
  if (argc != 2) {
    usage(argv[0]);
    return 2;
  }

  const std::string meta_path = argv[1];
  if (!ends_with(meta_path, ".meta.json")) {
    usage(argv[0]);
    return 2;
  }

  char* out = nullptr;
  size_t out_len = 0;

  const qcore_status_t rc = qcore_run_meta_file(meta_path.c_str(), &out, &out_len);

  if (!out) {
    std::cerr << "qcore_cli: no output\n";
    return 3;
  }

  std::cout.write(out, static_cast<std::streamsize>(out_len));
  std::cout << "\n";
  qcore_free(out);
  return static_cast<int>(rc);
}