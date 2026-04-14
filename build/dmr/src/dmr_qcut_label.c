/**
 * @file dmr_qcut_label.c
 * @brief QCut subcircuit labeling for DMR backend placement.
 *
 * @details
 * This module scans QCut subcircuit JSON artifacts, extracts lightweight circuit
 * metrics, and assigns a placement label to each fragment (QC/HPC/Undecided).
 *
 * The module supports multiple labeling policies:
 * - budget: threshold-based classification.
 * - score: continuous normalized score over QC budgets.
 * - hybrid: budget-first policy with score fallback.
 * - autobudget: global quota-based relabeling using baseline labels and score ranking.
 *
 * Generated outputs:
 * - Global CSV summary (`dmr_labels.csv`)
 * - One `.dmr_label` file per subcircuit
 *
 * Configuration is resolved from:
 * 1. Compiled defaults
 * 2. Optional config file
 * 3. Environment variable overrides
 *
 * @note
 * The `autobudget` mode is intended for low-variability fragment sets where
 * local thresholding collapses to a single dominant class.
 */

#include "dmr.h"
#include "dmr_internal.h"
#include "dmr_qcut_label.h"

#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

/**
 * @brief Label assigned to a subcircuit for execution placement.
 *
 * @details
 * The label indicates the preferred execution backend:
 * - QC: expected to be feasible/beneficial on a QPU.
 * - HPC: expected to be better executed on classical HPC resources.
 * - Undecided: insufficient or borderline information; requires policy resolution.
 */
typedef enum DMRQCutLabelEnum {
    DMR_QCUT_LABEL_QC = 0,          /**< Prefer QPU execution. */
    DMR_QCUT_LABEL_HPC = 1,         /**< Prefer classical HPC execution. */
    DMR_QCUT_LABEL_UNDECIDED = 2    /**< No strong recommendation. */
} DMRQCutLabel;

/**
 * @brief Metrics extracted from a QCut subcircuit JSON artifact.
 *
 * @details
 * Values may be -1 if the metric is missing or could not be parsed. The field
 * `has_any_metric` indicates whether at least one metric was successfully extracted.
 */
typedef struct DMRQCutMetricsStruct {
    int num_qubits;             /**< Parsed number of qubits, or -1 if unknown. */
    int depth;                  /**< Parsed circuit depth, or -1 if unknown. */
    int two_qubit_gates;        /**< Parsed 2-qubit gate count, or -1 if unknown. */
    int size_ops;               /**< Parsed total operations (Qiskit size), or -1 if unknown. */
    bool has_any_metric;        /**< True if at least one metric was parsed. */
} DMRQCutMetrics;

/**
 * @brief Ranking item used by the autobudget policy.
 *
 * @details
 * Stores the fragment index together with its precomputed QC pressure score
 * so that fragments can be globally sorted and reassigned.
 */
typedef struct DMRQCutRankItemStruct {
    size_t idx;   /**< Fragment index in the current batch. */
    double score; /**< Precomputed QC pressure score. */
} DMRQCutRankItem;

/**
 * @brief Threshold rules used to classify subcircuits as QC/HPC/Undecided.
 *
 * @details
 * The classification logic is intentionally conservative:
 * - HPC is selected if any known metric exceeds the HPC minimum thresholds.
 * - QC is selected only if the QC envelope is satisfied for required metrics.
 * - Otherwise, the label is Undecided.
 *
 * @note Values are loaded via `load_rules()` (defaults + optional config + env overrides).
 */
typedef struct DMRQCutRulesStruct {
    /* QC envelopes */
    int qc_max_qubits;  /**< Maximum qubits for QC classification. */
    int qc_max_depth;   /**< Maximum depth for QC classification. */
    int qc_max_2q;      /**< Maximum 2-qubit gates for QC classification. */
    int qc_max_size;    /**< Maximum total operations (size) for QC classification. */

    /* HPC triggers */
    int hpc_min_qubits; /**< Minimum qubits to force HPC classification. */
    int hpc_min_depth;  /**< Minimum depth to force HPC classification. */
    int hpc_min_2q;     /**< Minimum 2-qubit gates to force HPC classification. */
    int hpc_min_size;   /**< Minimum total operations (size) to force HPC classification. */

    /* Policy knobs */
    int hpc_votes_min; 

    /* Policy mode and score-based parameters */
    int label_mode;        /* 0=budget, 1=score, 2=hybrid, 3=autobudget */
    double score_tau_qc;   /* default 1.00 */
    double score_tau_hpc;  /* default 1.25 */
    double score_gap;      /* default 0.10 */
    double w_q, w_d, w_2q, w_sz;
} DMRQCutRules;

/* helpers */

/**
 * @brief Check whether a filesystem path exists and is a directory.
 *
 * @param path Filesystem path to test.
 * @return true if @p path is a directory, false otherwise.
 */
static bool path_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

/**
 * @brief Check whether a filesystem path exists and is a regular file.
 *
 * @param path Filesystem path to test.
 * @return true if @p path is a regular file, false otherwise.
 */
static bool path_is_regular(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

/**
 * @brief Test whether string @p s ends with suffix @p suffix.
 *
 * @param s Input string (may be NULL).
 * @param suffix Suffix to check (may be NULL).
 * @return true if @p s ends with @p suffix, false otherwise.
 */
static bool str_ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (lf > ls) return false;
    return (strncmp(s + (ls - lf), suffix, lf) == 0);
}

/**
 * @brief Join two path components with a single '/' separator.
 *
 * If @p a is empty, returns a duplicate of @p b.
 *
 * @param a Left path component (must not be NULL).
 * @param b Right path component (must not be NULL).
 * @return Newly allocated joined path (must be freed by caller), or NULL on invalid args.
 */
static char *join_path2(const char *a, const char *b)
{
    if (!a || !b) return NULL;
    char *out = NULL;
    if (a[0] == '\0') {
        asprintf_or_abort(&out, "%s", b);
        return out;
    }
    size_t la = strlen(a);
    if (a[la - 1] == '/')
        asprintf_or_abort(&out, "%s%s", a, b);
    else
        asprintf_or_abort(&out, "%s/%s", a, b);
    return out;
}

/**
 * @brief Read an integer value from an environment variable.
 *
 * @param[in]  env_name Environment variable name.
 * @param[out] out      Parsed integer value on success.
 *
 * @return true if the variable exists and was parsed as a valid int; false otherwise.
 */
static bool read_int_env(const char *env_name, int *out)
{
    if (!env_name || !out) return false;
    char *s = getenv(env_name);
    if (!s || s[0] == '\0') return false;

    char *endptr = NULL;
    errno = 0;
    long v = strtol(s, &endptr, 10);
    if (errno != 0 || endptr == s || *endptr != '\0') return false;
    if (v < INT_MIN || v > INT_MAX) return false;
    *out = (int)v;
    return true;
}

/**
 * @brief Read a double value from an environment variable.
 *
 * @param[in] key Environment variable name.
 * @param[out] dst Parsed double value on success.
 */
static void read_double_env(const char *key, double *dst)
{
    const char *v = getenv(key);
    if (!v || !v[0]) return;

    char *endptr = NULL;
    double val = strtod(v, &endptr);
    if (endptr != v) {
        *dst = val;
    }
}

/**
 * @brief Read a double value from an environment variable with fallback.
 *
 * @param[in] key Environment variable name.
 * @param[in] defval Default value returned when parsing fails or the variable is unset.
 * @return Parsed value or @p defval.
 */
static double read_double_env_or(const char *key, double defval)
{
    const char *v = getenv(key);
    if (!v || !v[0]) return defval;

    char *endptr = NULL;
    double val = strtod(v, &endptr);
    if (endptr == v) return defval;
    return val;
}

/**
 * @brief Load resolved labeling rules.
 *
 * @details
 * Rule resolution follows this precedence:
 * - compiled defaults,
 * - optional configuration file,
 * - environment variable overrides.
 *
 * Supported policies are:
 * - budget
 * - score
 * - hybrid
 * - autobudget
 *
 * @return Fully resolved rules structure.
 */
static void apply_config_file_if_present(DMRQCutRules *rules)
{
    if (!rules) return;

    char *cfg = getenv("DMR_QCUT_LABEL_CONFIG");
    if (!cfg || cfg[0] == '\0') return;

    FILE *f = fopen(cfg, "r");
    if (!f) {
        /* Non-fatal: configuration is optional */
        debug_output("DMR_QCUT_LABEL_CONFIG set but could not open file: %s\n", cfg);
        return;
    }

    char line[1024];
    while (fgets(line, (int)sizeof(line), f)) {
        /* trim leading whitespace */
        char *p = line;
        while (isspace((unsigned char)*p)) p++;

        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n') continue;

        /* find '=' */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        /* trim trailing spaces in key */
        char *kend = key + strlen(key) - 1;
        while (kend >= key && isspace((unsigned char)*kend)) { *kend = '\0'; kend--; }

        /* trim leading spaces in val */
        while (isspace((unsigned char)*val)) val++;

        /* trim trailing spaces/newlines in val */
        char *vend = val + strlen(val) - 1;
        while (vend >= val && (isspace((unsigned char)*vend) || *vend == '\n' || *vend == '\r')) { *vend = '\0'; vend--; }

        int ival = 0;
        char *endptr = NULL;
        errno = 0;
        long v = strtol(val, &endptr, 10);
        if (errno != 0 || endptr == val || *endptr != '\0' || v < INT_MIN || v > INT_MAX) {
            continue;
        }
        ival = (int)v;

        if (strcmp(key, "qc_max_qubits") == 0) rules->qc_max_qubits = ival;
        else if (strcmp(key, "qc_max_depth") == 0) rules->qc_max_depth = ival;
        else if (strcmp(key, "qc_max_2q") == 0) rules->qc_max_2q = ival;
        else if (strcmp(key, "hpc_min_qubits") == 0) rules->hpc_min_qubits = ival;
        else if (strcmp(key, "hpc_min_depth") == 0) rules->hpc_min_depth = ival;
        else if (strcmp(key, "hpc_min_2q") == 0) rules->hpc_min_2q = ival;
    }

    fclose(f);
}

/**
 * @brief Load labeling rules using defaults, optional config file, and env overrides.
 *
 * @details
 * Precedence:
 * - compiled defaults (`dmr_qcut_label.h`)
 * - overrides from config file (`DMR_QCUT_LABEL_CONFIG`)
 * - overrides from environment variables (highest)
 *
 * @return Fully resolved rules.
 */
static DMRQCutRules load_rules(void)
{
    DMRQCutRules r;

    /* ---- defaults ---- */
    r.qc_max_qubits = DMR_QCUT_QC_MAX_QUBITS;
    r.qc_max_depth  = DMR_QCUT_QC_MAX_DEPTH;
    r.qc_max_2q     = DMR_QCUT_QC_MAX_2Q;

    r.hpc_min_qubits = DMR_QCUT_HPC_MIN_QUBITS;
    r.hpc_min_depth  = DMR_QCUT_HPC_MIN_DEPTH;
    r.hpc_min_2q     = DMR_QCUT_HPC_MIN_2Q;

    /* defaults */
    r.qc_max_size    = DMR_QCUT_QC_MAX_SIZE;
    r.hpc_min_size   = DMR_QCUT_HPC_MIN_SIZE;

    r.hpc_votes_min  = 2;
    r.label_mode     = 2; /* hybrid default */

    r.score_tau_qc   = 1.00;
    r.score_tau_hpc  = 1.25;
    r.score_gap      = 0.10;

    r.w_q  = 0.40;
    r.w_d  = 0.25;
    r.w_2q = 0.25;
    r.w_sz = 0.10;

    /* optional config file overrides */
    apply_config_file_if_present(&r);

    /* ---- ENV overrides (highest priority) ---- */

    read_int_env("DMR_QCUT_QC_MAX_QUBITS", &r.qc_max_qubits);
    read_int_env("DMR_QCUT_QC_MAX_DEPTH",  &r.qc_max_depth);
    read_int_env("DMR_QCUT_QC_MAX_2Q",     &r.qc_max_2q);

    read_int_env("DMR_QCUT_HPC_MIN_QUBITS", &r.hpc_min_qubits);
    read_int_env("DMR_QCUT_HPC_MIN_DEPTH",  &r.hpc_min_depth);
    read_int_env("DMR_QCUT_HPC_MIN_2Q",     &r.hpc_min_2q);

    /* size thresholds */
    read_int_env("DMR_QCUT_QC_MAX_SIZE",  &r.qc_max_size);
    read_int_env("DMR_QCUT_HPC_MIN_SIZE", &r.hpc_min_size);

    /* vote threshold */
    read_int_env("DMR_QCUT_HPC_VOTES_MIN", &r.hpc_votes_min);

    /* label mode */
    const char *mode = getenv("DMR_QCUT_LABEL_MODE");
    if (mode) {
        if (strcmp(mode, "budget") == 0) r.label_mode = 0;
        else if (strcmp(mode, "score") == 0) r.label_mode = 1;
        else if (strcmp(mode, "hybrid") == 0) r.label_mode = 2;
        else if (strcmp(mode, "autobudget") == 0) r.label_mode = 3;
        else r.label_mode = 2;
    }

    /* score doubles */
    read_double_env("DMR_QCUT_SCORE_TAU_QC",  &r.score_tau_qc);
    read_double_env("DMR_QCUT_SCORE_TAU_HPC", &r.score_tau_hpc);
    read_double_env("DMR_QCUT_SCORE_GAP",     &r.score_gap);

    read_double_env("DMR_QCUT_W_Q",  &r.w_q);
    read_double_env("DMR_QCUT_W_D",  &r.w_d);
    read_double_env("DMR_QCUT_W_2Q", &r.w_2q);
    read_double_env("DMR_QCUT_W_SZ", &r.w_sz);

    return r;
}

/**
 * @brief Read a text file into a NUL-terminated buffer.
 *
 * @param[in] path Path to the input file.
 * @param[in] max_bytes Maximum accepted file size.
 * @return Newly allocated text buffer, or NULL on error.
 */
static char *read_file_text(const char *path, size_t max_bytes)
{
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if ((size_t)sz > max_bytes) { fclose(f); return NULL; }
    rewind(f);

    char *buf = (char *)malloc_or_abort((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (rd != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';
    return buf;
}

/**
 * @brief Extract an integer field from a JSON text buffer.
 *
 * @details
 * This is a lightweight substring-based parser intended for stable metadata
 * files. It is not a full JSON parser.
 *
 * @param[in] json NUL-terminated JSON text.
 * @param[in] field Field name to extract.
 * @param[out] out Parsed integer value.
 * @return true if the field was found and parsed successfully, false otherwise.
 */
static bool json_extract_int(const char *json, const char *field, int *out)
{
    if (!json || !field || !out) return false;

    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", field);

    const char *p = strstr(json, needle);
    if (!p) return false;

    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    errno = 0;
    char *endptr = NULL;
    long v = strtol(p, &endptr, 10);
    if (errno != 0 || endptr == p) return false;
    if (v < INT_MIN || v > INT_MAX) return false;

    *out = (int)v;
    return true;
}

/**
 * @brief Parse subcircuit metrics from a JSON artifact.
 *
 * @details
 * The parser extracts the following metrics when available:
 * - number of qubits,
 * - depth,
 * - two-qubit gate count,
 * - total operation count (`size_ops`).
 *
 * Missing metrics are stored as `-1`.
 *
 * @param[in] json_path Path to the JSON artifact.
 * @return Parsed metrics structure.
 */
static DMRQCutMetrics parse_metrics_from_json(const char *json_path)
{
    DMRQCutMetrics m;
    m.num_qubits = -1;
    m.depth = -1;
    m.two_qubit_gates = -1;
    m.size_ops = -1;
    m.has_any_metric = false;

    char *txt = read_file_text(json_path, 2 * 1024 * 1024); /* 2MB */
    if (!txt) return m;

    int v = 0;

    if (json_extract_int(txt, "n_qubits", &v) ||
        json_extract_int(txt, "num_qubits", &v) ||
        json_extract_int(txt, "qubits", &v)) {
        m.num_qubits = v; m.has_any_metric = true;
    }
    if (json_extract_int(txt, "depth", &v)) {
        m.depth = v; m.has_any_metric = true;
    }
    if (json_extract_int(txt, "size_ops", &v)) {
        m.size_ops = v; m.has_any_metric = true;
    }

    if (json_extract_int(txt, "two_qubit_gates", &v) ||
        json_extract_int(txt, "twoq_gates", &v) ||
        json_extract_int(txt, "cnot_count", &v) ||
        json_extract_int(txt, "cx_count", &v)) {
        m.two_qubit_gates = v; m.has_any_metric = true;
    }

    free(txt);
    return m;
}

/* ------------------------ labeling ------------------------- */

/**
 * @brief Convert an internal label enum to its textual representation.
 *
 * @param[in] l Internal label value.
 * @return Constant string representation of the label.
 */
static const char *label_to_string(DMRQCutLabel l)
{
    switch (l) {
        case DMR_QCUT_LABEL_QC: return "QC";
        case DMR_QCUT_LABEL_HPC: return "HPC";
        default: return "Undecided";
    }
}

/**
 * @brief Compute a non-negative normalized ratio.
 *
 * @details
 * Missing metrics or disabled budgets yield zero contribution.
 *
 * @param[in] x Metric value.
 * @param[in] denom Normalization denominator.
 * @return Normalized ratio, or 0.0 when not applicable.
 */
static double clamp_pos_ratio(int x, int denom) {
    if (x < 0 || denom <= 0) return 0.0;
    return (double)x / (double)denom;
}

/**
 * @brief Check whether a fragment lies inside the QC admissible envelope.
 *
 * @details
 * QC classification requires qubit count plus at least one structural metric
 * (`depth`, `two_qubit_gates`, or `size_ops`). Active QC limits are enforced
 * only when the corresponding thresholds are strictly positive.
 *
 * @param[in] r Active labeling rules.
 * @param[in] m Parsed fragment metrics.
 * @return true if the fragment satisfies the QC envelope, false otherwise.
 */
static bool within_qc_envelope(const DMRQCutRules *r, const DMRQCutMetrics *m) {
    int q  = m->num_qubits;
    int d  = m->depth;
    int g2 = m->two_qubit_gates;
    int sz = m->size_ops;

    bool has_q  = (q  >= 0);
    bool has_d  = (d  >= 0);
    bool has_g2 = (g2 >= 0);
    bool has_sz = (sz >= 0);

    bool has_struct = (has_d || has_g2 || has_sz);
    if (!has_q || !has_struct) return false;
    if (r->qc_max_qubits > 0 && q > r->qc_max_qubits) return false;
    if (has_d  && r->qc_max_depth > 0 && d  > r->qc_max_depth) return false;
    if (has_g2 && r->qc_max_2q   > 0 && g2 > r->qc_max_2q) return false;
    if (has_sz && r->qc_max_size > 0 && sz > r->qc_max_size) return false;

    return true;
}

/**
 * @brief Count how many HPC trigger conditions are satisfied by a fragment.
 *
 * @param[in] r Active labeling rules.
 * @param[in] m Parsed fragment metrics.
 * @return Number of satisfied HPC trigger conditions.
 */
static int hpc_votes(const DMRQCutRules *r, const DMRQCutMetrics *m) {
    int q  = m->num_qubits;
    int d  = m->depth;
    int g2 = m->two_qubit_gates;
    int sz = m->size_ops;

    int votes = 0;
    if (q  >= 0 && r->hpc_min_qubits > 0 && q  >= r->hpc_min_qubits) votes++;
    if (d  >= 0 && r->hpc_min_depth  > 0 && d  >= r->hpc_min_depth)  votes++;
    if (g2 >= 0 && r->hpc_min_2q     > 0 && g2 >= r->hpc_min_2q)     votes++;
    if (sz >= 0 && r->hpc_min_size   > 0 && sz >= r->hpc_min_size)   votes++;
    return votes;
}

/**
 * @brief Compute the normalized QC pressure score of a fragment.
 *
 * @details
 * The score is a weighted combination of normalized circuit metrics with
 * respect to the configured QC budgets. Lower scores indicate stronger QC
 * affinity, while higher scores indicate stronger HPC affinity.
 *
 * @param[in] r Active labeling rules.
 * @param[in] m Parsed fragment metrics.
 * @return Continuous normalized score.
 */
static double qc_pressure_score(const DMRQCutRules *r, const DMRQCutMetrics *m) {
    double rq  = clamp_pos_ratio(m->num_qubits,      r->qc_max_qubits);
    double rd  = clamp_pos_ratio(m->depth,           r->qc_max_depth);
    double rg2 = clamp_pos_ratio(m->two_qubit_gates, r->qc_max_2q);
    double rsz = clamp_pos_ratio(m->size_ops,        r->qc_max_size);

    return r->w_q * rq + r->w_d * rd + r->w_2q * rg2 + r->w_sz * rsz;
}

/**
 * @brief Decide the local label of a fragment from rules and metrics.
 *
 * @details
 * The decision depends on the selected policy:
 * - budget: threshold-only classification
 * - score: score-only classification
 * - hybrid: budget-first with score fallback
 * - autobudget: local baseline used before global quota relabeling
 *
 * @param[in] r Active labeling rules.
 * @param[in] m Parsed fragment metrics.
 * @return Selected local label.
 */
static DMRQCutLabel decide_label(const DMRQCutRules *r, const DMRQCutMetrics *m)
{
    if (!r || !m || !m->has_any_metric) return DMR_QCUT_LABEL_UNDECIDED;

    if (r->label_mode == 0 || r->label_mode == 2 || r->label_mode == 3) {
        if (within_qc_envelope(r, m)) return DMR_QCUT_LABEL_QC;

        int votes = hpc_votes(r, m);
        if (votes >= (r->hpc_votes_min > 0 ? r->hpc_votes_min : 2)) {
            return DMR_QCUT_LABEL_HPC;
        }

        if (r->label_mode == 0) return DMR_QCUT_LABEL_UNDECIDED;
    }

    {
        double s = qc_pressure_score(r, m);
        double tau_qc  = r->score_tau_qc  > 0 ? r->score_tau_qc  : 1.00;
        double tau_hpc = r->score_tau_hpc > 0 ? r->score_tau_hpc : 1.25;
        double gap     = r->score_gap     > 0 ? r->score_gap     : 0.10;

        if (s <= (tau_qc - gap)) return DMR_QCUT_LABEL_QC;
        if (s >= (tau_hpc + gap)) return DMR_QCUT_LABEL_HPC;

        return DMR_QCUT_LABEL_UNDECIDED;
    }
}

/**
 * @brief Write the final label file of a subcircuit.
 *
 * @details
 * The output file stores traceability information, parsed metrics, and the
 * final placement label assigned by the labeler.
 *
 * @param[in] label_path Output label-file path.
 * @param[in] job_id Job identifier.
 * @param[in] sub_name Subcircuit name without extension.
 * @param[in] m Parsed metrics.
 * @param[in] l Final assigned label.
 * @return 0 on success, non-zero on error.
 */
static int write_label_file(const char *label_path, uint32_t job_id, const char *sub_name, const DMRQCutMetrics *m, DMRQCutLabel l)
{
    FILE *f = fopen(label_path, "w");
    if (!f) return 1;

    fprintf(f, "job_id=%" PRIu32 "\n", job_id);
    fprintf(f, "subcircuit=%s\n", sub_name ? sub_name : "UNKNOWN");
    fprintf(f, "label=%s\n", label_to_string(l));
    fprintf(f, "num_qubits=%d\n", m ? m->num_qubits : -1);
    fprintf(f, "depth=%d\n", m ? m->depth : -1);
    fprintf(f, "two_qubit_gates=%d\n", m ? m->two_qubit_gates : -1);
    fprintf(f, "size_ops=%d\n", m ? m->size_ops : -1);

    fclose(f);
    return 0;
}

/**
 * @brief Compare two ranking items by ascending score.
 *
 * @param[in] a Pointer to first ranking item.
 * @param[in] b Pointer to second ranking item.
 * @return Negative, zero, or positive value following qsort conventions.
 */
static int cmp_rankitem_score_asc(const void *a, const void *b)
{
    const DMRQCutRankItem *x = (const DMRQCutRankItem *)a;
    const DMRQCutRankItem *y = (const DMRQCutRankItem *)b;

    if (x->score < y->score) return -1;
    if (x->score > y->score) return 1;
    if (x->idx < y->idx) return -1;
    if (x->idx > y->idx) return 1;
    return 0;
}

/**
 * @brief Resolve autobudget target counts from configured percentages.
 *
 * @details
 * The target distribution is controlled through:
 * - `DMR_QCUT_AUTOBUDGET_QC_PCT`
 * - `DMR_QCUT_AUTOBUDGET_HPC_PCT`
 * - `DMR_QCUT_AUTOBUDGET_UNDEF_PCT`
 *
 * Percentages are normalized if needed and converted into exact fragment counts.
 *
 * @param[in] n Total number of fragments.
 * @param[out] target_qc Target number of QC labels.
 * @param[out] target_hpc Target number of HPC labels.
 * @param[out] target_und Target number of Undecided labels.
 */
static void autobudget_target_counts(size_t n,
                                     size_t *target_qc,
                                     size_t *target_hpc,
                                     size_t *target_und)
{
    double p_qc  = read_double_env_or("DMR_QCUT_AUTOBUDGET_QC_PCT", 0.30);
    double p_hpc = read_double_env_or("DMR_QCUT_AUTOBUDGET_HPC_PCT", 0.30);
    double p_und = read_double_env_or("DMR_QCUT_AUTOBUDGET_UNDEF_PCT", 0.40);

    if (p_qc < 0.0) p_qc = 0.0;
    if (p_hpc < 0.0) p_hpc = 0.0;
    if (p_und < 0.0) p_und = 0.0;

    double s = p_qc + p_hpc + p_und;
    if (s <= 0.0) {
        p_qc = 0.30;
        p_hpc = 0.30;
        p_und = 0.40;
        s = 1.0;
    }

    p_qc  /= s;
    p_hpc /= s;
    p_und /= s;

    size_t tq = (size_t)(p_qc  * (double)n + 0.5);
    size_t th = (size_t)(p_hpc * (double)n + 0.5);
    size_t tu = (size_t)(p_und * (double)n + 0.5);

    size_t total = tq + th + tu;

    if (total < n) {
        tu += (n - total);
    } else if (total > n) {
        size_t extra = total - n;

        if (tu >= extra) {
            tu -= extra;
        } else {
            extra -= tu;
            tu = 0;

            if (th >= extra) {
                th -= extra;
            } else {
                extra -= th;
                th = 0;

                if (tq >= extra) {
                    tq -= extra;
                } else {
                    tq = 0;
                }
            }
        }
    }

    *target_qc  = tq;
    *target_hpc = th;
    *target_und = n - tq - th;
}

/**
 * @brief Label QCut subcircuits and generate DMR placement artifacts.
 *
 * @details
 * This function scans the configured subcircuits directory, parses fragment
 * metrics from JSON metadata, computes local labels, and optionally applies
 * the global `autobudget` relabeling policy. It then emits:
 * - one `.dmr_label` file per fragment,
 * - one CSV summary file for the whole job.
 *
 * The CSV includes the following metrics:
 * - `num_qubits`
 * - `depth`
 * - `two_qubit_gates`
 * - `size` (exported from internal `size_ops`)
 *
 * @param[in] job_id Job identifier used for traceability.
 * @return 0 on success, non-zero error code on failure.
 *
 * @retval 2 Output directory does not exist or is not a directory.
 * @retval 3 Subcircuits directory does not exist or is not a directory.
 * @retval 4 CSV output file could not be created.
 * @retval 5 Subcircuits directory could not be opened.
 * @retval 6 Memory allocation failure while collecting input JSON files.
 */
static void apply_autobudget_policy(const DMRQCutRules *rules,
                                    const DMRQCutMetrics *metrics,
                                    const DMRQCutLabel *baseline_labels,
                                    DMRQCutLabel *final_labels,
                                    const double *scores,
                                    size_t n)
{
    if (!rules || !metrics || !baseline_labels || !final_labels || !scores || n == 0)
        return;

    size_t target_qc = 0, target_hpc = 0, target_und = 0;
    autobudget_target_counts(n, &target_qc, &target_hpc, &target_und);

    DMRQCutRankItem *all_items = (DMRQCutRankItem *)malloc_or_abort(n * sizeof(DMRQCutRankItem));
    DMRQCutRankItem *qc_pref_items = (DMRQCutRankItem *)malloc_or_abort(n * sizeof(DMRQCutRankItem));
    bool *assigned = (bool *)calloc(n, sizeof(bool));
    if (!assigned) {
        dmr_error("QCut labeler: out of memory in apply_autobudget_policy\n");
        free(all_items);
        free(qc_pref_items);
        return;
    }

    size_t qc_pref_count = 0;
    for (size_t i = 0; i < n; ++i) {
        all_items[i].idx = i;
        all_items[i].score = scores[i];
        final_labels[i] = DMR_QCUT_LABEL_UNDECIDED;

        bool strong_qc = false;
        if (baseline_labels[i] == DMR_QCUT_LABEL_QC) {
            strong_qc = true;
        } else if (within_qc_envelope(rules, &metrics[i])) {
            strong_qc = true;
        }

        if (strong_qc) {
            qc_pref_items[qc_pref_count].idx = i;
            qc_pref_items[qc_pref_count].score = scores[i];
            qc_pref_count++;
        }
    }

    qsort(all_items, n, sizeof(DMRQCutRankItem), cmp_rankitem_score_asc);
    qsort(qc_pref_items, qc_pref_count, sizeof(DMRQCutRankItem), cmp_rankitem_score_asc);

    size_t qc_assigned = 0;
    for (size_t k = 0; k < qc_pref_count && qc_assigned < target_qc; ++k) {
        size_t idx = qc_pref_items[k].idx;
        if (!assigned[idx]) {
            final_labels[idx] = DMR_QCUT_LABEL_QC;
            assigned[idx] = true;
            qc_assigned++;
        }
    }

    for (size_t k = 0; k < n && qc_assigned < target_qc; ++k) {
        size_t idx = all_items[k].idx;
        if (!assigned[idx]) {
            final_labels[idx] = DMR_QCUT_LABEL_QC;
            assigned[idx] = true;
            qc_assigned++;
        }
    }

    size_t hpc_assigned = 0;
    for (size_t k = 0; k < n && hpc_assigned < target_hpc; ++k) {
        size_t idx = all_items[n - 1 - k].idx;
        if (!assigned[idx]) {
            final_labels[idx] = DMR_QCUT_LABEL_HPC;
            assigned[idx] = true;
            hpc_assigned++;
        }
    }

    for (size_t i = 0; i < n; ++i) {
        if (!assigned[i]) {
            final_labels[i] = DMR_QCUT_LABEL_UNDECIDED;
        }
    }

    free(assigned);
    free(all_items);
    free(qc_pref_items);

    debug_output("QCut labeler AUTOBUDGET: targets qc=%zu hpc=%zu und=%zu applied over %zu fragments\n",
                 target_qc, target_hpc, target_und, n);
}

/**
 * @brief Label QCut subcircuits for DMR consumption and emit CSV and label files.
 *
 * @details
 * This function resolves the output directory and subcircuits directory using:
 * - `DMR_QCUT_OUTPUT_DIR` (default: `DMR_QCUT_OUTPUT_DIR_DEFAULT`)
 * - `DMR_QCUT_SUBCIRCUITS_DIR` (optional; if unset defaults to `<output_dir>/subcircuits`)
 *
 * It scans `*.json` artifacts in the subcircuits directory, extracts metrics,
 * writes one label file per JSON, and writes a CSV summary in `<output_dir>`.
 *
 * @param[in] job_id SLURM job id used only for traceability in generated outputs.
 *
 * @return 0 on success; non-zero on error.
 * @retval 2 Output directory missing or not a directory.
 * @retval 3 Subcircuits directory missing or not a directory.
 * @retval 4 CSV output could not be created.
 * @retval 5 Subcircuits directory could not be opened.
 */
int dmr_qcut_label_job(uint32_t job_id)
{
    char *out_dir = getenv("DMR_QCUT_OUTPUT_DIR");
    if (!out_dir || out_dir[0] == '\0') {
        out_dir = (char *)DMR_QCUT_OUTPUT_DIR_DEFAULT;
    }

    char *subdir = getenv("DMR_QCUT_SUBCIRCUITS_DIR");
    char *sub_path = NULL;

    if (subdir && subdir[0] != '\0') {
        sub_path = strdup_or_abort(subdir);
    } else {
        char *tmp = join_path2(out_dir, DMR_QCUT_SUBDIR_DEFAULT);
        sub_path = tmp;
    }

    if (!path_is_dir(out_dir)) {
        dmr_error("QCut labeler: output dir does not exist or is not a directory: %s\n", out_dir);
        free(sub_path);
        return 2;
    }

    if (!path_is_dir(sub_path)) {
        dmr_error("QCut labeler: subcircuits dir does not exist or is not a directory: %s\n", sub_path);
        free(sub_path);
        return 3;
    }

    DMRQCutRules rules = load_rules();

    char *csv_path = join_path2(out_dir, DMR_QCUT_LABELS_CSV_DEFAULT);
    FILE *csv = fopen(csv_path, "w");
    if (!csv) {
        dmr_error("QCut labeler: could not open CSV output: %s\n", csv_path);
        free(csv_path);
        free(sub_path);
        return 4;
    }

    fprintf(csv, "job_id,subcircuit,label,num_qubits,depth,two_qubit_gates,size,source_json\n");

    DIR *d = opendir(sub_path);
    if (!d) {
        dmr_error("QCut labeler: could not open subcircuits dir: %s\n", sub_path);
        fclose(csv);
        free(csv_path);
        free(sub_path);
        return 5;
    }

    char **json_files = NULL;
    size_t json_count = 0;
    size_t json_cap   = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (!str_ends_with(ent->d_name, ".json"))
            continue;

        char *json_path = join_path2(sub_path, ent->d_name);
        if (!json_path)
            continue;

        if (!path_is_regular(json_path)) {
            free(json_path);
            continue;
        }
        free(json_path);

        if (json_count == json_cap) {
            size_t new_cap = (json_cap == 0) ? 16 : json_cap * 2;
            char **tmp = realloc(json_files, new_cap * sizeof(char *));
            if (!tmp) {
                dmr_error("QCut labeler: out of memory while collecting json files\n");
                closedir(d);
                fclose(csv);
                free(csv_path);
                free(sub_path);
                for (size_t i = 0; i < json_count; ++i) free(json_files[i]);
                free(json_files);
                return 6;
            }
            json_files = tmp;
            json_cap   = new_cap;
        }

        json_files[json_count++] = strdup_or_abort(ent->d_name);
    }

    closedir(d);

    DMRQCutMetrics *metrics = NULL;
    DMRQCutLabel *baseline_labels = NULL;
    DMRQCutLabel *final_labels = NULL;
    double *scores = NULL;

    if (json_count > 0) {
        metrics = (DMRQCutMetrics *)malloc_or_abort(json_count * sizeof(DMRQCutMetrics));
        baseline_labels = (DMRQCutLabel *)malloc_or_abort(json_count * sizeof(DMRQCutLabel));
        final_labels = (DMRQCutLabel *)malloc_or_abort(json_count * sizeof(DMRQCutLabel));
        scores = (double *)malloc_or_abort(json_count * sizeof(double));
    }

    /* -------- first pass: parse + baseline -------- */
    for (size_t i = 0; i < json_count; ++i) {
        const char *json_name = json_files[i];

        char *json_path = join_path2(sub_path, json_name);
        if (!json_path) {
            metrics[i].num_qubits = -1;
            metrics[i].depth = -1;
            metrics[i].two_qubit_gates = -1;
            metrics[i].size_ops = -1;
            metrics[i].has_any_metric = false;
            baseline_labels[i] = DMR_QCUT_LABEL_UNDECIDED;
            final_labels[i] = DMR_QCUT_LABEL_UNDECIDED;
            scores[i] = 0.0;
            continue;
        }

        metrics[i] = parse_metrics_from_json(json_path);
        baseline_labels[i] = decide_label(&rules, &metrics[i]);
        final_labels[i] = baseline_labels[i];
        scores[i] = qc_pressure_score(&rules, &metrics[i]);

        free(json_path);
    }

    /* -------- second pass: autobudget global relabel -------- */
    if (rules.label_mode == 3 && json_count > 0) {
        apply_autobudget_policy(&rules,
                                metrics,
                                baseline_labels,
                                final_labels,
                                scores,
                                json_count);
    }

    int labeled = 0;

    for (size_t i = 0; i < json_count; ++i) {
        const char *json_name = json_files[i];

        char *base = strdup_or_abort(json_name);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';

        char *label_name = NULL;
        asprintf_or_abort(&label_name, "%s%s", base, DMR_QCUT_LABEL_FILE_EXT);
        char *label_path = join_path2(sub_path, label_name);

        if (write_label_file(label_path, job_id, base, &metrics[i], final_labels[i]) == 0) {
            labeled++;
        }

        fprintf(csv, "%" PRIu32 ",%s,%s,%d,%d,%d,%d,%s\n",
            job_id,
            base,
            label_to_string(final_labels[i]),
            metrics[i].num_qubits,
            metrics[i].depth,
            metrics[i].two_qubit_gates,
            metrics[i].size_ops,
            json_name);

        free(label_path);
        free(label_name);
        free(base);
    }

    for (size_t i = 0; i < json_count; ++i) {
        free(json_files[i]);
    }
    free(json_files);

    free(metrics);
    free(baseline_labels);
    free(final_labels);
    free(scores);

    fclose(csv);

    debug_output("QCut labeler: wrote %d labels. CSV: %s\n", labeled, csv_path);

    free(csv_path);
    free(sub_path);

    return 0;
}
