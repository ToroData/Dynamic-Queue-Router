#include "backends/backend_common.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <sstream>

#include <nlohmann/json.hpp>

namespace qcore {

namespace {

static std::string read_pipe(FILE* pipe) {
    std::string result;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        result += buf.data();
    }
    return result;
}


static std::string extract_json_block(const std::string& raw) {
    auto first = raw.find('{');
    auto last  = raw.rfind('}');
    if (first != std::string::npos && last != std::string::npos && last > first) {
        return raw.substr(first, last - first + 1);
    }
    return raw;
}


static std::string resolve_qpu_script_path() {
    const char* env = std::getenv("QCORE_QPU_SCRIPT");
    if (env && env[0] != '\0') {
        return std::string(env);
    }

#if defined(__linux__)
    {
        FILE* maps = std::fopen("/proc/self/maps", "r");
        if (maps) {
            char line[1024];
            while (std::fgets(line, sizeof(line), maps)) {
                std::string s(line);
                if (s.find("libqcore") != std::string::npos) {
                    auto slash = s.rfind('/');
                    if (slash != std::string::npos) {
                        std::string dir = s.substr(s.find('/'), slash - s.find('/') + 1);
                        std::fclose(maps);
                        return dir + "backend_qpu_qulacs.py";
                    }
                }
            }
            std::fclose(maps);
        }
    }

    {
        char exe[4096] = {};
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            std::string path(exe, static_cast<size_t>(n));
            auto slash = path.rfind('/');
            if (slash != std::string::npos) {
                return path.substr(0, slash + 1) + "backend_qpu_qulacs.py";
            }
        }
    }
#endif

    return "backend_qpu_qulacs.py";
}

static std::string python_interpreter() {
    const char* env = std::getenv("QCORE_QPU_PYTHON");
    return (env && env[0] != '\0') ? std::string(env) : "python3";
}

} // anonymous namespace

BackendRunResult run_qpu_qulacs(const nlohmann::json& meta, BackendInfo& info) {
    info.selected = "qpu_qulacs";
    info.device   = "qpu";
    info.gpus     = 0;

    if (!meta.contains("qasm_path") || !meta["qasm_path"].is_string()) {
        throw std::runtime_error("run_qpu_qulacs: falta campo requerido 'qasm_path' en meta");
    }

    std::string meta_path;
    if (meta.contains("_meta_file_path_") && meta["_meta_file_path_"].is_string()) {
        meta_path = meta["_meta_file_path_"].get<std::string>();
    } else {
        std::string tmppath = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                              + "/qcore_qpu_meta_XXXXXX.json";
        std::vector<char> tmpbuf(tmppath.begin(), tmppath.end());
        tmpbuf.push_back('\0');

#if defined(__linux__)
        int fd = mkstemps(tmpbuf.data(), 5 /* len(".json") */);
        if (fd < 0) {
            throw std::runtime_error("run_qpu_qulacs: cannot create temporary file for meta JSON");
        }
        meta_path = std::string(tmpbuf.data());
        {
            FILE* f = fdopen(fd, "w");
            if (!f) {
                close(fd);
                throw std::runtime_error("run_qpu_qulacs: cannot open temporary file for meta JSON");
            }
            const std::string meta_text = meta.dump(2);
            std::fputs(meta_text.c_str(), f);
            std::fclose(f);
        }
#else
        throw std::runtime_error(
            "run_qpu_qulacs: requires '_meta_file_path_' in meta "
            "when there is no support for mkstemps (non-Linux)"
        );
#endif
    }

    const std::string qasm_path = meta["qasm_path"].get<std::string>();
    const std::string script    = resolve_qpu_script_path();
    const std::string python    = python_interpreter();

    std::ostringstream cmd;
    cmd << python << " "
        << "'" << script    << "' "
        << "'" << meta_path << "' "
        << "'" << qasm_path << "' "
        << "2>&1";

    auto t0 = std::chrono::steady_clock::now();

    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) {
        throw std::runtime_error(
            "run_qpu_qulacs: cannot launch Python process: " + cmd.str()
        );
    }

    const std::string output = read_pipe(pipe);
    int rc = pclose(pipe);

    auto t1 = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!meta.contains("_meta_file_path_")) {
        std::remove(meta_path.c_str());
    }

    if (output.empty()) {
        throw std::runtime_error(
            "run_qpu_qulacs: the Python script did not produce output (rc=" +
            std::to_string(rc) + ")"
        );
    }

    nlohmann::json jout;
    try {
        jout = nlohmann::json::parse(output);
    } catch (const nlohmann::json::exception&) {
        const std::string json_block = extract_json_block(output);
        try {
            jout = nlohmann::json::parse(json_block);
        } catch (const nlohmann::json::exception& e2) {
            throw std::runtime_error(
                std::string("run_qpu_qulacs: invalid JSON from Python script: ") +
                e2.what() + "\nRaw output: " + output.substr(0, 512)
            );
        }
    }

    if (jout.value("status", "error") != "ok") {
        const std::string msg = jout.contains("error")
            ? jout["error"].value("message", "unknown error in QPU backend")
            : "unknown error in QPU backend";
        throw std::runtime_error("run_qpu_qulacs: " + msg);
    }

    BackendRunResult r;

    if (jout.contains("results")) {
        const auto& res = jout["results"];
        r.expected_value = res.value("expected_value", 0.0);
        r.shots          = res.value("shots", 0);
        r.pauli          = res.value("pauli", std::string(""));
    }

    if (jout.contains("timing_ms")) {
        const auto& tm = jout["timing_ms"];
        r.simulate_ms    = tm.value("simulate", elapsed_ms);
        r.observables_ms = tm.value("observables", 0.0);
    } else {
        r.simulate_ms = elapsed_ms;
    }

    if (jout.contains("cut")) {
        const auto& cut = jout["cut"];
        const std::string ctype = cut.value("type", "wire");
        r.cut_type = (ctype == "gate") ? CutType::Gate : CutType::Wire;
        if (r.cut_type == CutType::Gate) {
            r.gate_cut_index = cut.value("term_index", -1);
            r.gate_cut_sign  = cut.value("term_sign",  +1);
        }
    }

    return r;
}

} // namespace qcore
