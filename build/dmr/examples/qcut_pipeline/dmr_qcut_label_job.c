/**
 * @file dmr_qcut_label_job.c
 * @brief CLI wrapper to run dmr_qcut_label_job() from shell scripts
 */

#include "dmr.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <job_id>\n", argv[0]);
        fprintf(stderr, "Environment:\n");
        fprintf(stderr, "  DMR_QCUT_OUTPUT_DIR=... (default: ./output)\n");
        fprintf(stderr, "  DMR_QCUT_SUBCIRCUITS_DIR=... (optional)\n");
        fprintf(stderr, "  DMR_QCUT_LABEL_CONFIG=... (optional)\n");
        return 2;
    }

    const char *s = argv[1];

    while (*s && (*s < '0' || *s > '9')) {
        s++;
    }

    if (!*s) {
        fprintf(stderr, "Invalid job_id (no digits found): %s\n", argv[1]);
        fflush(stderr);
        return 3;
    }

    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);

    if (!end || *end != '\0') {
        fprintf(stderr, "Invalid job_id. Trailing non-digits: %s\n", argv[1]);
        fflush(stderr);
        return 3;
    }

    return dmr_qcut_label_job((uint32_t)v);
}
