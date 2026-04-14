#include "qtensor/qtensor_api.h"
#include "qtensor/qtensor.h"

#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>
#include <exception>

#include <nlohmann/json.hpp>

static char* qtensor_strdup_malloc(const std::string& s) {
    char* p = (char*)std::malloc(s.size() + 1);
    if (!p) return nullptr;
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

extern "C" {

void qtensor_free(void* p) {
    std::free(p);
}

int qtensor_reconstruct(
    const double* ev,
    size_t        n_ev,
    qtensor_cut_type_t cut_type,
    int           number_cuts,
    int64_t       number_components,
    qtensor_result_t* out,
    char**        error_msg
) {
    if (error_msg) *error_msg = nullptr;

    if (!ev || n_ev == 0 || !out) {
        if (error_msg) *error_msg = qtensor_strdup_malloc("qtensor_reconstruct: invalid arguments");
        return 1;
    }

    try {
        std::vector<double> v(ev, ev + n_ev);

        qtensor::CutType ct =
            (cut_type == QTENSOR_CUT_GATE) ? qtensor::CutType::GATE : qtensor::CutType::WIRE;
        auto r = qtensor::reconstruct(v, ct, number_cuts, number_components);

        out->R = r.R;
        out->base = r.base;
        out->terms = r.terms;
        out->m = r.m;
        out->n = r.n;
        out->m_inferred = r.m_inferred ? 1 : 0;

        return 0;
    } catch (const std::exception& e) {
        if (error_msg) *error_msg = qtensor_strdup_malloc(e.what());
        return 2;
    } catch (...) {
        if (error_msg) *error_msg = qtensor_strdup_malloc("qtensor_reconstruct: unknown exception");
        return 3;
    }
}

int qtensor_reconstruct_json(
    const double* ev,
    size_t        n_ev,
    qtensor_cut_type_t cut_type,
    int           number_cuts,
    int64_t       number_components,
    const char*   job_id,
    const char*   fragment_id,
    char**        out_json,
    char**        error_msg
) {
    if (error_msg) *error_msg = nullptr;
    if (out_json)  *out_json  = nullptr;

    qtensor_result_t rr{};
    char* em = nullptr;

    int rc = qtensor_reconstruct(ev, n_ev, cut_type, number_cuts, number_components, &rr, &em);
    if (rc != 0) {
        if (error_msg) *error_msg = em; else qtensor_free(em);
        return rc;
    }

    try {
        nlohmann::json j;
        j["schema"] = "qtensor.reconstruction.v1";
        j["job_id"] = (job_id && job_id[0]) ? nlohmann::json(job_id) : nullptr;
        j["fragment_id"] = (fragment_id && fragment_id[0]) ? nlohmann::json(fragment_id) : nullptr;

        j["cut_type"] = (cut_type == QTENSOR_CUT_GATE) ? "GATE" : "WIRE";
        j["number_cuts"] = number_cuts;
        j["base"] = rr.base;
        j["terms"] = rr.terms;
        j["number_components"] = rr.m;
        j["m_inferred"] = (rr.m_inferred != 0);
        j["n_ev"] = rr.n;
        j["global_factor"] = 1.0 / std::ldexp(1.0, number_cuts);
        j["result_R"] = rr.R;

        std::string s = j.dump(2);
        char* p = qtensor_strdup_malloc(s);
        if (!p) {
            if (error_msg) *error_msg = qtensor_strdup_malloc("qtensor_reconstruct_json: OOM");
            return 4;
        }
        *out_json = p;
        return 0;
    } catch (const std::exception& e) {
        if (error_msg) *error_msg = qtensor_strdup_malloc(e.what());
        return 5;
    } catch (...) {
        if (error_msg) *error_msg = qtensor_strdup_malloc("qtensor_reconstruct_json: unknown exception");
        return 6;
    }
}

} // extern "C"
