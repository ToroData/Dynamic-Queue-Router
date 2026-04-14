/**
 * @file run_qcore_batch.c
 * @brief Batch runner for QCore fragments using in-process public C APIs:
 *        - qcore_run_meta_file() for fragment simulation
 *        - qtensor_reconstruct_json() for reconstruction
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>

#include "qcore/qcore.h"
#include "qtensor/qtensor_api.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/**
 * @brief Cut type as encoded in `partition.json`.
 */
typedef enum { CUT_GATE = 0, CUT_WIRE = 1 } CutType;

/**
 * @brief Fragment descriptor discovered on disk.
 *
 * @note `qasm_path` is used for an optional sanity check only; QCore infers the
 * paired QASM path internally from the meta path.
 */
typedef struct {
    int index;
    char meta_path[PATH_MAX];
    char qasm_path[PATH_MAX];
} FragItem;

/**
 * @brief Return non-zero if @p path exists and is a regular file.
 * @param path Filesystem path.
 * @return 1 if regular file exists, otherwise 0.
 */
static int is_file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

/**
 * @brief Read entire file into a NUL-terminated heap buffer.
 * @param path Filesystem path.
 * @param[out] out_size Optional output size in bytes (excluding NUL).
 * @return Pointer to malloc'ed buffer on success; NULL on failure.
 * @note Caller must free() the returned buffer.
 */
static char *read_entire_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    char *buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) { free(buf); return NULL; }
    buf[n] = '\0';
    if (out_size) *out_size = (size_t)n;
    return buf;
}

/**
 * @brief Minimal JSON boolean field parser (substring-based).
 * @param json Input JSON buffer.
 * @param field Field pattern including quotes (e.g., "\"gate_cut\"").
 * @param[out] out_val Parsed value (0/1).
 * @return 1 on success, 0 on failure.
 * @warning Not a general JSON parser; assumes simple formatting.
 */
static int parse_bool_field(const char *json, const char *field, int *out_val) {
    const char *p = strstr(json, field);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "true", 4) == 0)  { *out_val = 1; return 1; }
    if (strncmp(p, "false", 5) == 0) { *out_val = 0; return 1; }
    return 0;
}

/**
 * @brief Minimal JSON integer field parser (substring-based).
 * @param json Input JSON buffer.
 * @param field Field pattern including quotes (e.g., "\"k_cuts\"").
 * @param[out] out_val Parsed integer.
 * @return 1 on success, 0 on failure.
 * @warning Not a general JSON parser; assumes simple formatting.
 */
static int parse_int_field(const char *json, const char *field, int *out_val) {
    const char *p = strstr(json, field);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out_val = (int)v;
    return 1;
}

/**
 * @brief Minimal JSON string equality check for a field (substring-based).
 * @param json Input JSON buffer.
 * @param field Field pattern including quotes (e.g., "\"method\"").
 * @param value Expected value (no quotes).
 * @return 1 if exact match, otherwise 0.
 * @warning Not a general JSON parser; assumes simple formatting and no escapes.
 */
static int parse_string_field_equals(const char *json, const char *field, const char *value) {
    const char *p = strstr(json, field);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return 0;
    p++;
    size_t lv = strlen(value);
    if (strncmp(p, value, lv) != 0) return 0;
    if (p[lv] != '"') return 0;
    return 1;
}

/**
 * @brief Extract `"expected_value"` from a QCore JSON response.
 * @param resp_json Response JSON string.
 * @param[out] ok Set to 1 on success, 0 on failure (optional).
 * @return Parsed value, or 0.0 on failure.
 * @warning Substring-based numeric parse; assumes `"expected_value": <number>`.
 */
static double parse_expected_value_from_response(const char *resp_json, int *ok) {
    const char *p = strstr(resp_json, "\"expected_value\"");
    if (!p) { if (ok) *ok = 0; return 0.0; }
    p = strchr(p, ':');
    if (!p) { if (ok) *ok = 0; return 0.0; }
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) { if (ok) *ok = 0; return 0.0; }
    if (ok) *ok = 1;
    return v;
}

/**
 * @brief Parse fragment index from `<prefix><digits><suffix>`.
 * @param filename Directory entry name.
 * @param prefix Expected prefix (e.g., "frag_").
 * @param suffix Expected suffix (e.g., ".meta.json").
 * @return Parsed integer index on success; -1 on failure.
 */
static int parse_frag_index(const char *filename, const char *prefix, const char *suffix) {
    size_t lp = strlen(prefix), ls = strlen(suffix), ln = strlen(filename);
    if (ln < lp + 1 + ls) return -1;
    if (strncmp(filename, prefix, lp) != 0) return -1;
    if (strcmp(filename + ln - ls, suffix) != 0) return -1;

    size_t start = lp, end = ln - ls;
    char numbuf[32]; size_t k = 0;
    for (size_t i = start; i < end && k < sizeof(numbuf)-1; i++) {
        if (!isdigit((unsigned char)filename[i])) return -1;
        numbuf[k++] = filename[i];
    }
    numbuf[k] = '\0';
    if (k == 0) return -1;
    return atoi(numbuf);
}

/**
 * @brief qsort comparator for FragItem by index (ascending).
 * @param a Pointer to FragItem.
 * @param b Pointer to FragItem.
 * @return Negative/zero/positive according to ordering.
 */
static int compare_frag_item(const void *a, const void *b) {
    const FragItem *x = (const FragItem*)a;
    const FragItem *y = (const FragItem*)b;
    return (x->index - y->index);
}

/**
 * @brief Convert qcore_status_t to a stable string for logs.
 * @param st Status code.
 * @return Static string label.
 */
static const char* qcore_status_str(qcore_status_t st) {
    switch (st) {
        case QCORE_OK:        return "QCORE_OK";
        case QCORE_EINVALID:  return "QCORE_EINVALID";
        case QCORE_EBACKEND:  return "QCORE_EBACKEND";
        case QCORE_EOOM:      return "QCORE_EOOM";
        case QCORE_ERUNTIME:  return "QCORE_ERUNTIME";
        default:              return "QCORE_EUNKNOWN";
    }
}

/**
 * @brief Batch entrypoint.
 *
 * @param argc Argument count.
 * @param argv Arguments:
 *  - argv[1]: job_output_dir
 *  - argv[2]: out_jsonl (one JSON object per line)
 *  - argv[3]: optional recon_json_name (default: qtensor_reconstruction.json)
 *
 * @return 0 on success; non-zero on error.
 *
 * @details
 * Uses:
 *  - `qcore_run_meta_file()` per fragment.
 *  - `qtensor_reconstruct_json()` for reconstruction.
 *
 * Ownership:
 *  - `qcore_run_meta_file()` returns a buffer released via `qcore_free()`.
 *  - `qtensor_reconstruct_json()` returns buffers released via `qtensor_free()`.
 */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <job_output_dir> <out_jsonl> [recon_json_name]\n", argv[0]);
        return 2;
    }

    const char *job_out_dir = argv[1];
    const char *out_jsonl   = argv[2];
    const char *recon_name  = (argc >= 4) ? argv[3] : "qtensor_reconstruction.json";

    char sub_dir[PATH_MAX];
    snprintf(sub_dir, sizeof(sub_dir), "%s/subcircuits", job_out_dir);

    char partition_path[PATH_MAX];
    snprintf(partition_path, sizeof(partition_path), "%s/partition.json", job_out_dir);

    char *partition = read_entire_file(partition_path, NULL);
    if (!partition) {
        fprintf(stderr, "ERROR: cannot read partition: %s (%s)\n", partition_path, strerror(errno));
        return 1;
    }

    CutType cut_type = CUT_GATE;
    if (parse_string_field_equals(partition, "\"method\"", "GATE")) {
        cut_type = CUT_GATE;
    } else if (parse_string_field_equals(partition, "\"method\"", "WIRE")) {
        cut_type = CUT_WIRE;
    } else {
        int gate_cut = 0;
        if (parse_bool_field(partition, "\"gate_cut\"", &gate_cut)) {
            cut_type = gate_cut ? CUT_GATE : CUT_WIRE;
        } else {
            fprintf(stderr, "ERROR: cannot determine cut type from partition.json (expected method: GATE|WIRE)\n");
            free(partition);
            return 1;
        }
    }

    int k = -1;
    if (!parse_int_field(partition, "\"k_cuts\"", &k)) {
        if (!parse_int_field(partition, "\"num_cuts\"", &k)) {
            fprintf(stderr, "ERROR: cannot determine number_cuts (k). Provide it in partition.json as k_cuts\n");
            free(partition);
            return 1;
        }
    }
    free(partition);

    if (k < 0) {
        fprintf(stderr, "ERROR: invalid k_cuts=%d in partition.json\n", k);
        return 1;
    }

    DIR *d = opendir(sub_dir);
    if (!d) {
        fprintf(stderr, "ERROR: cannot open dir: %s (%s)\n", sub_dir, strerror(errno));
        return 1;
    }

    size_t cap = 32, nitems = 0;
    FragItem *items = (FragItem*)calloc(cap, sizeof(FragItem));
    if (!items) { closedir(d); return 1; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *fn = ent->d_name;
        int idx = parse_frag_index(fn, "frag_", ".meta.json");
        if (idx < 0) continue;

        if (nitems == cap) {
            cap *= 2;
            FragItem *tmp = (FragItem*)realloc(items, cap * sizeof(FragItem));
            if (!tmp) { closedir(d); free(items); return 1; }
            items = tmp;
        }

        FragItem it;
        memset(&it, 0, sizeof(it));
        it.index = idx;
        snprintf(it.meta_path, sizeof(it.meta_path), "%s/%s", sub_dir, fn);

        snprintf(it.qasm_path, sizeof(it.qasm_path), "%s/frag_%03d.qasm", sub_dir, idx);
        if (!is_file_exists(it.qasm_path)) {
            snprintf(it.qasm_path, sizeof(it.qasm_path), "%s/frag_%d.qasm", sub_dir, idx);
        }

        items[nitems++] = it;
    }
    closedir(d);

    if (nitems == 0) {
        fprintf(stderr, "ERROR: no frag_*.meta.json found in %s\n", sub_dir);
        free(items);
        return 1;
    }

    qsort(items, nitems, sizeof(FragItem), compare_frag_item);

    FILE *outf = fopen(out_jsonl, "wb");
    if (!outf) {
        fprintf(stderr, "ERROR: cannot open output: %s (%s)\n", out_jsonl, strerror(errno));
        free(items);
        return 1;
    }

    double *ev_vec = (double*)calloc(nitems, sizeof(double));
    if (!ev_vec) { fclose(outf); free(items); return 1; }

    for (size_t i = 0; i < nitems; i++) {
        if (!is_file_exists(items[i].qasm_path)) {
            fprintf(stderr, "ERROR: missing paired qasm for frag_%03d: %s\n",
                    items[i].index, items[i].qasm_path);
            fclose(outf);
            free(ev_vec);
            free(items);
            return 1;
        }

        char *resp = NULL;
        size_t resp_len = 0;

        qcore_status_t st = qcore_run_meta_file(items[i].meta_path, &resp, &resp_len);
        if (st != QCORE_OK || !resp) {
            fprintf(stderr, "ERROR: qcore_run_meta_file failed for frag_%03d (meta=%s) status=%s\n",
                    items[i].index, items[i].meta_path, qcore_status_str(st));
            if (resp) qcore_free(resp);
            fclose(outf);
            free(ev_vec);
            free(items);
            return 1;
        }

        for (size_t j = 0; j < resp_len; j++) {
            if (resp[j] == '\n' || resp[j] == '\r') resp[j] = ' ';
        }
        fwrite(resp, 1, resp_len, outf);
        fputc('\n', outf);

        int ok = 0;
        double ev = parse_expected_value_from_response(resp, &ok);
        if (!ok) {
            fprintf(stderr, "ERROR: cannot parse expected_value for frag_%03d\n", items[i].index);
            qcore_free(resp);
            fclose(outf);
            free(ev_vec);
            free(items);
            return 1;
        }

        ev_vec[i] = ev;
        fprintf(stderr, "[qcore] frag_%03d expected_value=%.12f\n", items[i].index, ev);

        qcore_free(resp);
    }

    fclose(outf);

    qtensor_cut_type_t ct = (cut_type == CUT_GATE) ? QTENSOR_CUT_GATE : QTENSOR_CUT_WIRE;
    int64_t number_components = -1;

    char *recon_json = NULL;
    char *qerr = NULL;

    int qrc = qtensor_reconstruct_json(
        ev_vec, nitems, ct, k, number_components,
        NULL, NULL,
        &recon_json, &qerr
    );

    if (qrc != 0) {
        fprintf(stderr, "ERROR: qtensor_reconstruct_json failed (rc=%d): %s\n",
                qrc, qerr ? qerr : "(no msg)");
        qtensor_free(qerr);
        free(ev_vec);
        free(items);
        return 1;
    }

    char outpath[PATH_MAX];
    snprintf(outpath, sizeof(outpath), "%s/%s", job_out_dir, recon_name);

    FILE *rf = fopen(outpath, "wb");
    if (!rf) {
        fprintf(stderr, "ERROR: cannot write %s (%s)\n", outpath, strerror(errno));
        qtensor_free(recon_json);
        free(ev_vec);
        free(items);
        return 1;
    }
    fwrite(recon_json, 1, strlen(recon_json), rf);
    fclose(rf);

    fprintf(stderr, "[qtensor] wrote reconstruction: %s\n", outpath);

    qtensor_free(recon_json);
    free(ev_vec);
    free(items);

    fprintf(stderr, "Done. Wrote JSONL: %s\n", out_jsonl);
    return 0;
}
