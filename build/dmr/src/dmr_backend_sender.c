/**
 * @file dmr_backend_sender.c
 * @brief MPI-based backend dispatch and reconstruction support for DMR.
 *
 * @details
 * This module implements the backend sender/worker layer used by DMR to:
 * - dispatch fragment simulation work to MPI workers,
 * - collect fragment completions,
 * - persist per-fragment result JSON files,
 * - reconstruct final results through the Qtensor C API.
 *
 * Rank 0 acts as dispatcher and completion collector, while worker ranks
 * execute fragment simulations through the QCore in-process API.
 *
 * The module maintains separate internal queues for QC and HPC routes and
 * applies a simple priority policy during dispatch.
 */
#include "dmr_backend_sender.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep(ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

#include "qcore/qcore.h"
#include "qtensor/qtensor_api.h"


/**
 * @brief Safe snprintf wrapper that always null-terminates the destination buffer.
 *
 * @param dst Destination buffer.
 * @param n Destination buffer size.
 * @param fmt Printf-style format string.
 * @param ... Format arguments.
 */
static void safe_snprintf(char *dst, size_t n, const char *fmt, ...) {
    if (!dst || n == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    dst[n-1] = '\0';
}

/**
 * @brief Create a directory path on a best-effort basis.
 *
 * @param dir Directory path.
 *
 * @return Non-zero on success, 0 on failure.
 *
 * @details
 * On POSIX systems this uses @c mkdir -p through the shell. On Windows it is
 * currently a no-op success path.
 */
static int mkdir_p_best_effort(const char *dir) {
    if (!dir || !dir[0]) return 0;
#if defined(_WIN32)
    (void)dir;
    return 0;
#else
    char cmd[1024];
    safe_snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\" >/dev/null 2>&1", dir);
    return system(cmd) == 0;
#endif
}

/**
 * @brief Write a text buffer into a file.
 *
 * @param path Output file path.
 * @param text Text content to write.
 *
 * @return 1 on success, 0 on failure.
 */
static int write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fputs(text ? text : "", f);
    fclose(f);
    return 1;
}

/**
 * @brief Return a monotonic timestamp in milliseconds.
 *
 * @return Current monotonic time in milliseconds.
 */

static int64_t g_sender_t0_ms  = 0;
static int     g_sender_t0_set = 0;

static double dqr_sender_elapsed_s(void);

static int64_t now_ms_monotonic(void) {
#if defined(_WIN32)
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}

/**
 * @brief Return seconds elapsed since first call (lazy init for Gantt logging).
 */
static double dqr_sender_elapsed_s(void) {
    int64_t now = now_ms_monotonic();
    if (!g_sender_t0_set) {
        g_sender_t0_ms  = now;
        g_sender_t0_set = 1;
    }
    return (double)(now - g_sender_t0_ms) / 1000.0;
}

/**
 * @brief Convert a QCore status code into a printable string.
 *
 * @param st QCore status code.
 *
 * @return Constant string representation of the status.
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
 * @brief Parse the expected value field from a JSON response.
 *
 * @param resp_json Input JSON text.
 * @param out_ev Output parsed expected value.
 *
 * @return 1 on success, 0 on failure.
 */
static int parse_expected_value(const char *resp_json, double *out_ev) {
    if (!resp_json || !out_ev) return 0;
    const char *p = strstr(resp_json, "\"expected_value\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out_ev = v;
    return 1;
}

/**
 * @brief Map a QCore status code to a DMR completion status.
 *
 * @param st QCore status code.
 *
 * @return Corresponding DMR completion status.
 *
 * @details
 * Invalid input is treated as permanent failure, while backend/runtime/resource
 * failures are treated as transient.
 */
static int completion_status_from_qcore(qcore_status_t st) {
    switch (st) {
        case QCORE_OK:       return DMR_COMP_DONE;
        case QCORE_EINVALID: return DMR_COMP_FAIL_PERMANENT;
        case QCORE_EBACKEND: return DMR_COMP_FAIL_TRANSIENT;
        case QCORE_EOOM:     return DMR_COMP_FAIL_TRANSIENT;
        case QCORE_ERUNTIME: return DMR_COMP_FAIL_TRANSIENT;
        default:             return DMR_COMP_FAIL_TRANSIENT;
    }
}

typedef struct {
    char frag_id[DMR_MAX_FRAG_ID];
    int route; // DMRRoute
    int kind;  // DMRWorkKind
} DMRWorkMsg;

typedef struct {
    char frag_id[DMR_MAX_FRAG_ID];
    int route;
    int kind;
} PendingItem;

typedef struct {
    PendingItem *buf;
    size_t cap;
    size_t head;
    size_t tail;
    size_t size;
} RingQ;

typedef struct {
    int idx;
    char path[PATH_MAX];
} FragResItem;

/**
 * @brief Initialize a ring queue with a fixed capacity.
 *
 * @param q Queue instance.
 * @param cap Queue capacity.
 *
 * @return 1 on success, 0 on failure.
 */
static int ringq_init(RingQ *q, size_t cap) {
    if (!q || cap == 0) return 0;
    memset(q, 0, sizeof(*q));
    q->buf = (PendingItem*)calloc(cap, sizeof(PendingItem));
    if (!q->buf) return 0;
    q->cap = cap;
    return 1;
}

/**
 * @brief Release memory used by a ring queue.
 *
 * @param q Queue instance.
 */
static void ringq_free(RingQ *q) {
    if (!q) return;
    free(q->buf);
    memset(q, 0, sizeof(*q));
}

/**
 * @brief Push one item into the ring queue.
 *
 * @param q Queue instance.
 * @param it Item to insert.
 *
 * @return 1 on success, 0 on failure or full queue.
 */
static int ringq_push(RingQ *q, const PendingItem *it) {
    if (!q || !q->buf || !it) return 0;
    if (q->size == q->cap) return 0;
    q->buf[q->tail] = *it;
    q->tail = (q->tail + 1) % q->cap;
    q->size++;
    return 1;
}

/**
 * @brief Pop one item from the ring queue.
 *
 * @param q Queue instance.
 * @param out Output item.
 *
 * @return 1 on success, 0 on failure or empty queue.
 */
static int ringq_pop(RingQ *q, PendingItem *out) {
    if (!q || !q->buf || !out) return 0;
    if (q->size == 0) return 0;
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->size--;
    return 1;
}

// Keep two separate queues (QC/HPC) as requested
static RingQ g_qc_q;
static RingQ g_hpc_q;
static int g_queues_ready = 0;

// counts of currently-dispatched-but-not-done items by route (rank 0)
static int g_inflight_qc = 0;
static int g_inflight_hpc = 0;

/**
 * @brief Build the metadata path for a fragment.
 *
 * @param dst Output buffer.
 * @param n Output buffer size.
 * @param job_dir Job base directory.
 * @param frag_id Fragment identifier.
 */
static void build_meta_path(char *dst, size_t n, const char *job_dir, const char *frag_id) {
    // Expect frag_id like "frag_000"
    safe_snprintf(dst, n, "%s/subcircuits/%s.meta.json", job_dir, frag_id);
}

/**
 * @brief Build the result JSON path for a fragment.
 *
 * @param dst Output buffer.
 * @param n Output buffer size.
 * @param results_dir Results directory.
 * @param frag_id Fragment identifier.
 */
static void build_result_path(char *dst, size_t n, const char *results_dir, const char *frag_id) {
    safe_snprintf(dst, n, "%s/%s.json", results_dir, frag_id);
}

/**
 * @brief Persist one completion result to disk when enabled.
 *
 * @param s Backend sender instance.
 * @param c Completion record.
 */
static void sender_write_result(DMRBackendSender *s, const DMRCompletion *c) {
    if (!s || !c || !s->enable_results_write) return;
    char path[DMR_MAX_PATH];
    build_result_path(path, sizeof(path), s->results_dir, c->frag_id);
    write_text_file(path, c->result_json);
}

/**
 * @brief Initialize the backend sender context.
 *
 * @param s Backend sender instance to initialize.
 * @param comm MPI communicator used for dispatch and collection.
 * @param job_dir Base job directory containing subcircuit metadata.
 * @param results_dir Directory where fragment result files are written.
 *
 * @return 1 on success, 0 on failure.
 *
 * @details
 * Rank 0 initializes canonical paths, worker bookkeeping and internal queues.
 * Configuration is broadcast to all ranks so workers can resolve fragment paths.
 */
int dmr_backend_sender_init(DMRBackendSender *s, MPI_Comm comm,
                            const char *job_dir, const char *results_dir)
{
    if (!s) return 0;
    memset(s, 0, sizeof(*s));
    s->comm = comm;

    MPI_Comm_rank(comm, &s->world_rank);
    MPI_Comm_size(comm, &s->world_size);

    if (s->world_rank == 0) {
        if (job_dir && job_dir[0]) {
            strncpy(s->job_dir, job_dir, sizeof(s->job_dir)-1);
            s->job_dir[sizeof(s->job_dir)-1] = '\0';
        } else {
            s->job_dir[0] = '\0';
        }

        if (results_dir && results_dir[0]) {
            strncpy(s->results_dir, results_dir, sizeof(s->results_dir)-1);
            s->results_dir[sizeof(s->results_dir)-1] = '\0';
        } else {
            safe_snprintf(s->results_dir, sizeof(s->results_dir), "%s/results", s->job_dir);
        }

        s->enable_results_write = 1;
        s->qc_slots = 0;

        mkdir_p_best_effort(s->results_dir);
    }

    // Broadcast config to all ranks (workers need job_dir)
    MPI_Bcast(s->job_dir, (int)sizeof(s->job_dir), MPI_BYTE, 0, comm);
    MPI_Bcast(s->results_dir, (int)sizeof(s->results_dir), MPI_BYTE, 0, comm);
    MPI_Bcast(&s->enable_results_write, 1, MPI_INT, 0, comm);
    MPI_Bcast(&s->qc_slots, 1, MPI_INT, 0, comm);

    // Rank 0 worker pool bookkeeping
    if (s->world_rank == 0) {
        s->worker_busy = (int*)calloc((size_t)s->world_size, sizeof(int));
        if (!s->worker_busy) return 0;
        for (int r = 1; r < s->world_size; ++r) s->worker_busy[r] = 0;

        // queues
        if (!g_queues_ready) {
            if (!ringq_init(&g_qc_q, 4096)) return 0;
            if (!ringq_init(&g_hpc_q, 4096)) return 0;
            g_queues_ready = 1;
        }

        g_inflight_qc = 0;
        g_inflight_hpc = 0;
    }

    return 1;
}

/**
 * @brief Finalize the backend sender context.
 *
 * @param s Backend sender instance.
 *
 * @details
 * Releases rank-0 worker bookkeeping and clears the sender state.
 */
void dmr_backend_sender_finalize(DMRBackendSender *s) {
    if (!s) return;

    if (s->world_rank == 0) {
        free(s->worker_busy);
        s->worker_busy = NULL;
    }

    memset(s, 0, sizeof(*s));
}

/**
 * @brief Execute one fragment simulation task on a worker rank.
 *
 * @param s Backend sender instance.
 * @param w Work descriptor received from rank 0.
 * @param c Output completion record.
 *
 * @details
 * The function resolves the fragment metadata path, invokes QCore on the
 * fragment metadata file, measures runtime, stores the response JSON and
 * extracts the fragment expected value when available.
 */
static void worker_run_fragment(DMRBackendSender *s, const DMRWorkMsg *w, DMRCompletion *c) {
    // Build meta path from job_dir + frag_id
    char meta_path[DMR_MAX_PATH];
    build_meta_path(meta_path, sizeof(meta_path), s->job_dir, w->frag_id);

    int64_t t0 = now_ms_monotonic();

    char *resp = NULL;
    size_t resp_len = 0;

    // qcore_status_t st = qcore_run_meta_file(meta_path, &resp, &resp_len);
    qcore_status_t st = qcore_run_meta_file_ex(meta_path, w->route, &resp, &resp_len);

    int64_t t1 = now_ms_monotonic();
    c->t_ms = (int)(t1 - t0);

    c->status = completion_status_from_qcore(st);

    if (resp && resp_len > 0) {
        for (size_t i = 0; i < resp_len; i++) {
            if (resp[i] == '\n' || resp[i] == '\r') resp[i] = ' ';
        }
        size_t to_copy = resp_len;
        if (to_copy >= sizeof(c->result_json)) to_copy = sizeof(c->result_json) - 1;
        memcpy(c->result_json, resp, to_copy);
        c->result_json[to_copy] = '\0';

        if (st == QCORE_OK) {
            double ev = 0.0;
            if (parse_expected_value(c->result_json, &ev)) {
                c->expected_value = ev;
            } else {
                c->expected_value = 0.0;
            }
        } else {
            c->expected_value = 0.0;
        }
    } else {
        c->expected_value = 0.0;
        safe_snprintf(
            c->result_json, sizeof(c->result_json),
            "{"
            "\"schema_version\":\"dmr.backend_sender.error.v1\","
            "\"frag_id\":\"%s\","
            "\"meta_path\":\"%s\","
            "\"qcore_status\":\"%s\""
            "}",
            w->frag_id, meta_path, qcore_status_str(st)
        );
    }

    if (resp) qcore_free(resp);
}

/**
 * @brief Run the MPI worker loop.
 *
 * @param comm MPI communicator shared with the dispatcher.
 *
 * @details
 * Worker ranks wait for work messages from rank 0, execute supported tasks,
 * and send completion records back to the dispatcher. The loop terminates
 * when a stop message is received.
 */
void dmr_backend_worker_loop(MPI_Comm comm) {
    DMRBackendSender s;
    if (!dmr_backend_sender_init(&s, comm, NULL, NULL)) {
        return;
    }

    for (;;) {
        MPI_Status st;
        int rc = MPI_Probe(0, MPI_ANY_TAG, comm, &st);
        if (rc != MPI_SUCCESS) break;

        if (st.MPI_TAG == DMR_TAG_STOP) {
            MPI_Recv(NULL, 0, MPI_BYTE, 0, DMR_TAG_STOP, comm, MPI_STATUS_IGNORE);
            break;
        }

        if (st.MPI_TAG == DMR_TAG_WORK) {
            DMRWorkMsg w;
            memset(&w, 0, sizeof(w));
            MPI_Recv(&w, (int)sizeof(w), MPI_BYTE, 0, DMR_TAG_WORK, comm, MPI_STATUS_IGNORE);

            DMRCompletion c;
            memset(&c, 0, sizeof(c));
            strncpy(c.frag_id, w.frag_id, sizeof(c.frag_id)-1);
            c.route = w.route;
            c.kind  = w.kind;

            if (w.kind == DMR_WORK_FRAGMENT_SIM) {
                worker_run_fragment(&s, &w, &c);
            } else {
                c.status = DMR_COMP_FAIL_PERMANENT;
                safe_snprintf(
                    c.result_json, sizeof(c.result_json),
                    "{"
                      "\"schema_version\":\"dmr.backend_sender.error.v1\","
                      "\"frag_id\":\"%s\","
                      "\"error\":\"unsupported_work_kind\""
                    "}",
                    w.frag_id
                );
            }

            MPI_Send(&c, (int)sizeof(c), MPI_BYTE, 0, DMR_TAG_DONE, comm);
            continue;
        }

        int count = 0;
        MPI_Get_count(&st, MPI_BYTE, &count);
        if (count > 0) {
            void *buf = malloc((size_t)count);
            MPI_Recv(buf, count, MPI_BYTE, 0, st.MPI_TAG, comm, MPI_STATUS_IGNORE);
            free(buf);
        } else {
            MPI_Recv(NULL, 0, MPI_BYTE, 0, st.MPI_TAG, comm, MPI_STATUS_IGNORE);
        }
    }

    dmr_backend_sender_finalize(&s);
}

/**
 * @brief Find an idle worker rank.
 *
 * @param s Backend sender instance.
 *
 * @return Worker rank on success, or -1 if none is available.
 */
static int find_free_worker(DMRBackendSender *s) {
    if (!s || s->world_rank != 0) return -1;
    for (int r = 1; r < s->world_size; ++r) {
        if (s->worker_busy[r] == 0) return r;
    }
    return -1;
}

/**
 * @brief Check whether one QC task can be dispatched under the current slot limit.
 *
 * @param s Backend sender instance.
 *
 * @return Non-zero if QC dispatch is allowed, 0 otherwise.
 */
static int can_dispatch_qc(DMRBackendSender *s) {
    if (!s) return 0;
    if (s->qc_slots <= 0) return 1; // unlimited
    return (g_inflight_qc < s->qc_slots);
}

/**
 * @brief Dispatch one queued task to a free worker if possible.
 *
 * @param s Backend sender instance.
 *
 * @return 1 if one task was dispatched, 0 otherwise.
 *
 * @details
 * QC tasks are prioritized over HPC tasks when QC slot constraints allow it.
 */
static int dispatch_one_if_possible(DMRBackendSender *s) {
    if (!s || s->world_rank != 0) return 0;

    int w = find_free_worker(s);
    if (w < 1) return 0;

    PendingItem it;
    memset(&it, 0, sizeof(it));

    // Priority policy:
    // 1) QC queue if slots available
    // 2) else HPC queue
    // 3) else QC (even if slots limited) as last resort? (no)
    int picked = 0;
    if (g_qc_q.size > 0 && can_dispatch_qc(s)) {
        picked = ringq_pop(&g_qc_q, &it);
    }
    if (!picked && g_hpc_q.size > 0) {
        picked = ringq_pop(&g_hpc_q, &it);
    }
    if (!picked) return 0;

    DMRWorkMsg msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.frag_id, it.frag_id, sizeof(msg.frag_id)-1);
    msg.route = it.route;
    msg.kind  = it.kind;

    MPI_Send(&msg, (int)sizeof(msg), MPI_BYTE, w, DMR_TAG_WORK, s->comm);
    s->worker_busy[w] = 1;

    /* Gantt log: fragment execution started (sent to worker via MPI) */
    {
        static int s_dbg_checked = 0, s_dbg_level = 0;
        if (!s_dbg_checked) {
            const char *v = getenv("DMR_QCUT_DQR_DEBUG");
            if (v && *v) s_dbg_level = atoi(v);
            s_dbg_checked = 1;
        }
        if (s_dbg_level >= 1) {
            fprintf(stderr, "[RANK0] T=%.3f START frag_id=%s\n",
                    dqr_sender_elapsed_s(), it.frag_id);
            fflush(stderr);
        }
    }

    if (it.route == DMR_ROUTE_QC) g_inflight_qc++;
    else                         g_inflight_hpc++;

    return 1;
}

/**
 * @brief Submit one fragment simulation task to the internal dispatch queues.
 *
 * @param s Backend sender instance. Must be called on rank 0.
 * @param frag_id Fragment identifier.
 * @param route Execution route associated with the fragment.
 *
 * @return 1 if the task was queued successfully, 0 otherwise.
 *
 * @details
 * The fragment is enqueued into the QC or HPC queue depending on the route.
 * Dispatch to free workers is attempted immediately after submission.
 */
int dmr_backend_sender_submit_one(DMRBackendSender *s, const char *frag_id, int route) {
    if (!s || s->world_rank != 0 || !frag_id || !frag_id[0]) return 0;
    if (!g_queues_ready) return 0;

    PendingItem it;
    memset(&it, 0, sizeof(it));
    strncpy(it.frag_id, frag_id, sizeof(it.frag_id)-1);
    it.route = route;
    it.kind  = DMR_WORK_FRAGMENT_SIM;

    int ok = 0;
    if (route == DMR_ROUTE_QC) ok = ringq_push(&g_qc_q, &it);
    else                      ok = ringq_push(&g_hpc_q, &it);

    if (!ok) return 0;

    while (dispatch_one_if_possible(s)) { 

     }

    return 1;
}

/**
 * @brief Poll completed fragment executions from MPI workers.
 *
 * @param s Backend sender instance. Must be called on rank 0.
 * @param out Output buffer for collected completions.
 * @param max_out Maximum number of completions to retrieve.
 *
 * @return Number of completions written into @p out.
 *
 * @details
 * This function receives available completion messages, marks workers as idle,
 * updates route inflight counters, optionally persists result JSON files, and
 * triggers additional dispatch if pending work exists.
 */
int dmr_backend_sender_poll(DMRBackendSender *s, DMRCompletion *out, int max_out) {
    if (!s || s->world_rank != 0 || !out || max_out <= 0) return 0;

    int n = 0;
    while (n < max_out) {
        int flag = 0;
        MPI_Status st;
        MPI_Iprobe(MPI_ANY_SOURCE, DMR_TAG_DONE, s->comm, &flag, &st);
        if (!flag) break;

        DMRCompletion c;
        memset(&c, 0, sizeof(c));
        MPI_Recv(&c, (int)sizeof(c), MPI_BYTE, st.MPI_SOURCE, DMR_TAG_DONE, s->comm, MPI_STATUS_IGNORE);

        // mark worker idle
        if (st.MPI_SOURCE >= 1 && st.MPI_SOURCE < s->world_size) {
            s->worker_busy[st.MPI_SOURCE] = 0;
        }

        // update inflight counters
        if (c.route == DMR_ROUTE_QC) {
            if (g_inflight_qc > 0) g_inflight_qc--;
        } else {
            if (g_inflight_hpc > 0) g_inflight_hpc--;
        }

        sender_write_result(s, &c);

        // return completion to caller (DQR/DMR will ACK/store)
        out[n++] = c;

        // after freeing worker, try dispatch more
        while (dispatch_one_if_possible(s)) { /* refill */ }
    }

    // also try dispatch even if no completions (e.g. just queued work)
    while (dispatch_one_if_possible(s)) { /* refill */ }

    return n;
}

/**
 * @brief Broadcast a stop message to all worker ranks.
 *
 * @param s Backend sender instance. Must be called on rank 0.
 *
 * @details
 * Sends a termination message to every worker rank so their worker loop exits
 * cleanly.
 */
void dmr_backend_sender_broadcast_stop(DMRBackendSender *s) {
    if (!s || s->world_rank != 0) return;
    for (int r = 1; r < s->world_size; ++r) {
        MPI_Send(NULL, 0, MPI_BYTE, r, DMR_TAG_STOP, s->comm);
    }
}

/**
 * @brief Extract a string field from a flat JSON object.
 *
 * @param json Input JSON text.
 * @param field Field name to search for.
 * @param out Output buffer.
 * @param out_n Output buffer size.
 *
 * @return 1 on success, 0 on failure.
 */
static int parse_json_string_field(const char *json, const char *field, char *out, size_t out_n) {
    if (!json || !field || !out || out_n == 0) return 0;
    const char *p = strstr(json, field);
    if (!p) return 0;

    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return 0;
    p++; // start of value

    const char *q = strchr(p, '"');
    if (!q) return 0;

    size_t len = (size_t)(q - p);
    if (len >= out_n) len = out_n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/**
 * @brief Reconstruct a final result from fragment expected values using Qtensor.
 *
 * @param ev_vec Input vector of fragment expected values.
 * @param nitems Number of entries in @p ev_vec.
 * @param cut_type Cut type identifier (0 = gate, 1 = wire).
 * @param k_cuts Number of cuts.
 * @param number_components Number of partition components, or inferred value.
 * @param job_id Optional job identifier.
 * @param recon_json Output reconstructed JSON allocated by Qtensor.
 * @param err_json Output error JSON allocated by Qtensor.
 *
 * @return Qtensor return code.
 *
 * @details
 * Thin wrapper around the public Qtensor reconstruction API. Output buffers
 * are allocated by Qtensor and must be released with @c qtensor_free().
 */
int dmr_backend_sender_qtensor_reconstruct(
    const double *ev_vec,
    size_t nitems,
    int cut_type /*0=GATE,1=WIRE*/,
    int k_cuts,
    int64_t number_components,
    const char *job_id,
    char **recon_json,
    char **err_json
) {
    if (!ev_vec || nitems == 0 || !recon_json || !err_json) return -1;

    qtensor_cut_type_t ct = (cut_type == 0) ? QTENSOR_CUT_GATE : QTENSOR_CUT_WIRE;

    int rc = qtensor_reconstruct_json(
        (double*)ev_vec, nitems,
        ct,
        k_cuts,
        number_components,
        job_id, NULL,
        recon_json,
        err_json
    );

    return rc;
}


#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/**
 * @brief Check whether a path exists and is a regular file.
 *
 * @param path File path.
 *
 * @return Non-zero if the file exists and is regular, 0 otherwise.
 */
static int file_exists_regular(const char *path) {
    struct stat st;
    if (!path) return 0;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

/**
 * @brief Read a whole local file into memory.
 *
 * @param path File path.
 * @param out_size Optional pointer to store file size.
 *
 * @return Newly allocated null-terminated buffer, or NULL on failure.
 */
static char *read_entire_file_local(const char *path, size_t *out_size) {
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
 * @brief Parse cut type and number of cuts from a partition JSON.
 *
 * @param partition_json Partition JSON text.
 * @param out_cut_type Output cut type (gate or wire).
 * @param out_k Output number of cuts.
 *
 * @return 1 on success, 0 on failure.
 */
static int parse_partition_cut_and_k(const char *partition_json, int *out_cut_type, int *out_k) {
    if (!partition_json || !out_cut_type || !out_k) return 0;

    // default
    int cut_type = 1; // WIRE
    int k = -1;

    const char *p = NULL;

    // method
    p = strstr(partition_json, "\"method\"");
    if (p) {
        const char *c = strchr(p, ':');
        if (c) {
            c++;
            while (*c && isspace((unsigned char)*c)) c++;
            if (*c == '"') {
                c++;
                if (strncmp(c, "GATE", 4) == 0) cut_type = 0;
                else if (strncmp(c, "WIRE", 4) == 0) cut_type = 1;
            }
        }
    } else {
        // gate_cut fallback
        p = strstr(partition_json, "\"gate_cut\"");
        if (!p) return 0;
        const char *c = strchr(p, ':');
        if (!c) return 0;
        c++;
        while (*c && isspace((unsigned char)*c)) c++;
        if (strncmp(c, "true", 4) == 0) cut_type = 0;
        else if (strncmp(c, "false", 5) == 0) cut_type = 1;
        else return 0;
    }

    // k_cuts / num_cuts
    p = strstr(partition_json, "\"k_cuts\"");
    if (!p) p = strstr(partition_json, "\"num_cuts\"");
    if (!p) return 0;
    const char *c = strchr(p, ':');
    if (!c) return 0;
    c++;
    while (*c && isspace((unsigned char)*c)) c++;
    char *end = NULL;
    long vv = strtol(c, &end, 10);
    if (end == c) return 0;
    k = (int)vv;
    if (k < 0) return 0;

    *out_cut_type = cut_type;
    *out_k = k;
    return 1;
}

/**
 * @brief Parse a fragment index from a result filename.
 *
 * @param filename Result filename.
 *
 * @return Parsed fragment index, or -1 if the filename is not valid.
 *
 * @details
 * Accepted formats include @c frag_<digits>.json and
 * @c frag_<digits>.result.json.
 */
static int parse_frag_index_any(const char *filename) {
    if (!filename) return -1;
    const char *p = filename;

    if (strncmp(p, "frag_", 5) != 0) return -1;
    p += 5;

    // digits
    int val = 0;
    int nd = 0;
    while (*p && isdigit((unsigned char)*p)) {
        val = val * 10 + (*p - '0');
        p++;
        nd++;
    }
    if (nd == 0) return -1;

    // accept ".json" or ".result.json"
    if (strcmp(p, ".json") == 0) return val;
    if (strcmp(p, ".result.json") == 0) return val;

    return -1;
}

/**
 * @brief Parse the expected value field from a JSON string.
 *
 * @param json Input JSON text.
 * @param out_ev Output parsed expected value.
 *
 * @return 1 on success, 0 on failure.
 */
static int parse_expected_value_from_json(const char *json, double *out_ev) {
    if (!json || !out_ev) return 0;
    const char *p = strstr(json, "\"expected_value\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out_ev = v;
    return 1;
}

/**
 * @brief Comparison helper for sorting fragment result items by index.
 *
 * @param a First item.
 * @param b Second item.
 *
 * @return Negative, zero or positive value following qsort semantics.
 */
static int cmp_frag_res_item(const void *a, const void *b) {
    const FragResItem *x = (const FragResItem*)a;
    const FragResItem *y = (const FragResItem*)b;
    return (x->idx - y->idx);
}

/**
 * @brief Reconstruct the final result from fragment JSON files stored on disk.
 *
 * @param job_dir Job directory containing partition metadata.
 * @param results_dir Directory containing fragment result JSON files.
 * @param recon_filename Output filename for the reconstructed JSON.
 * @param out_ev Optional pointer to store the reconstructed expected value.
 * @param out_recon_json Output reconstructed JSON allocated by Qtensor.
 * @param out_err_json Output error JSON allocated by this module or Qtensor.
 *
 * @return 0 on success, non-zero on error.
 *
 * @details
 * This function:
 * - reads partition metadata from @c partition.json,
 * - scans fragment result files in @p results_dir,
 * - extracts fragment expected values in index order,
 * - invokes Qtensor reconstruction,
 * - writes the reconstructed JSON into @p job_dir.
 *
 * Output buffers must be released by the caller with @c qtensor_free()
 * when applicable.
 */
int dmr_backend_sender_reconstruct_from_results(
    const char *job_dir,
    const char *results_dir,
    const char *recon_filename,
    double *out_ev,
    char **out_recon_json,
    char **out_err_json
) {
    if (out_ev) *out_ev = 0.0;
    if (!job_dir || !job_dir[0] || !results_dir || !results_dir[0] ||
        !recon_filename || !recon_filename[0] ||
        !out_recon_json || !out_err_json) {
        return -1;
    }
    *out_recon_json = NULL;
    *out_err_json = NULL;

    // read partition.json to get cut_type + k
    char partition_path[PATH_MAX];
    safe_snprintf(partition_path, sizeof(partition_path), "%s/partition.json", job_dir);

    char *partition = read_entire_file_local(partition_path, NULL);
    if (!partition) {
        char *ej = (char*)malloc(512);
        if (ej) {
            safe_snprintf(ej, 512,
                "{"
                  "\"schema_version\":\"dmr.backend_sender.error.v1\","
                  "\"error\":\"cannot_read_partition\","
                  "\"path\":\"%s\""
                "}", partition_path
            );
            *out_err_json = ej;
        }
        return -2;
    }

    char job_id_buf[256];
    job_id_buf[0] = '\0';
    parse_json_string_field(partition, "\"job_id\"", job_id_buf, sizeof(job_id_buf));
    const char *job_id = (job_id_buf[0] ? job_id_buf : NULL);

    int cut_type = 1; // WIRE default
    int k_cuts = -1;
    if (!parse_partition_cut_and_k(partition, &cut_type, &k_cuts)) {
        free(partition);
        char *ej = (char*)malloc(512);
        if (ej) {
            safe_snprintf(ej, 512,
                "{"
                  "\"schema_version\":\"dmr.backend_sender.error.v1\","
                  "\"error\":\"cannot_parse_partition\","
                  "\"path\":\"%s\""
                "}", partition_path
            );
            *out_err_json = ej;
        }
        return -3;
    }
    free(partition);

    // Scan results_dir for frag_*.json
    DIR *d = opendir(results_dir);
    if (!d) {
        char *ej = (char*)malloc(512);
        if (ej) {
            safe_snprintf(ej, 512,
                "{"
                  "\"schema_version\":\"dmr.backend_sender.error.v1\","
                  "\"error\":\"cannot_open_results_dir\","
                  "\"dir\":\"%s\","
                  "\"errno\":%d"
                "}", results_dir, errno
            );
            *out_err_json = ej;
        }
        return -4;
    }

    size_t cap = 64, n = 0;
    FragResItem *arr = (FragResItem*)calloc(cap, sizeof(FragResItem));
    if (!arr) { closedir(d); return -5; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *fn = ent->d_name;
        int idx = parse_frag_index_any(fn);
        if (idx < 0) continue;

        if (n == cap) {
            cap *= 2;
            FragResItem *tmp = (FragResItem*)realloc(arr, cap * sizeof(FragResItem));
            if (!tmp) { free(arr); closedir(d); return -6; }
            arr = tmp;
        }

        arr[n].idx = idx;
        safe_snprintf(arr[n].path, sizeof(arr[n].path), "%s/%s", results_dir, fn);
        n++;
    }
    closedir(d);

    if (n == 0) {
        free(arr);
        char *ej = (char*)malloc(512);
        if (ej) {
            safe_snprintf(ej, 512,
                "{"
                  "\"schema_version\":\"dmr.backend_sender.error.v1\","
                  "\"error\":\"no_frag_results_found\","
                  "\"dir\":\"%s\""
                "}", results_dir
            );
            *out_err_json = ej;
        }
        return -7;
    }

    qsort(arr, n, sizeof(FragResItem), cmp_frag_res_item);

    // Read each frag JSON, extract expected_value in order
    double *ev_vec = (double*)calloc(n, sizeof(double));
    if (!ev_vec) { free(arr); return -8; }

    for (size_t i = 0; i < n; i++) {
        char *txt = read_entire_file_local(arr[i].path, NULL);
        if (!txt) {
            char *ej = (char*)malloc(1024);
            if (ej) {
                safe_snprintf(ej, 1024,
                    "{"
                      "\"schema_version\":\"dmr.backend_sender.error.v1\","
                      "\"error\":\"cannot_read_result_file\","
                      "\"path\":\"%s\""
                    "}", arr[i].path
                );
                *out_err_json = ej;
            }
            free(ev_vec);
            free(arr);
            return -9;
        }

        double ev = 0.0;
        if (!parse_expected_value_from_json(txt, &ev)) {
            char *ej = (char*)malloc(1024);
            if (ej) {
                safe_snprintf(ej, 1024,
                    "{"
                      "\"schema_version\":\"dmr.backend_sender.error.v1\","
                      "\"error\":\"missing_expected_value\","
                      "\"path\":\"%s\""
                    "}", arr[i].path
                );
                *out_err_json = ej;
            }
            free(txt);
            free(ev_vec);
            free(arr);
            return -10;
        }
        free(txt);
        ev_vec[i] = ev;
    }

    // Call Qtensor reconstruction (infer components)
    int64_t number_components = -1;
    char *recon_json = NULL;
    char *err_json = NULL;

    int rc = dmr_backend_sender_qtensor_reconstruct(
        ev_vec, n,
        cut_type, k_cuts,
        number_components,
        job_id,
        &recon_json, &err_json
    );

    free(ev_vec);
    free(arr);

    if (rc != 0) {
        *out_err_json = err_json;
        if (recon_json) qtensor_free(recon_json);
        return rc;
    }

    // write recon to job_dir/recon_filename
    char outpath[PATH_MAX];
    safe_snprintf(outpath, sizeof(outpath), "%s/%s", job_dir, recon_filename);

    if (!write_text_file(outpath, recon_json)) {
        char *ej = (char*)malloc(1024);
        if (ej) {
            safe_snprintf(ej, 1024,
                "{"
                  "\"schema_version\":\"dmr.backend_sender.error.v1\","
                  "\"error\":\"cannot_write_reconstruction\","
                  "\"path\":\"%s\""
                "}", outpath
            );
            *out_err_json = ej;
        }
        *out_recon_json = recon_json;
        if (err_json) qtensor_free(err_json);
        return -11;
    }

    // extract expected_value from recon_json (optional)
    if (out_ev) {
        double ev_final = 0.0;
        if (parse_expected_value_from_json(recon_json, &ev_final)) {
            *out_ev = ev_final;
        } else {
            *out_ev = 0.0;
        }
    }

    if (err_json) qtensor_free(err_json);
    *out_recon_json = recon_json;
    *out_err_json = NULL;
    return 0;
}
