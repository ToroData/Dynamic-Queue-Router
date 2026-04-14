#include "dqr_slurm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int read_int_env(const char *k, int *out)
{
    const char *v = getenv(k);
    if(!v || !v[0]) return 0;
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if(end == v) return 0;
    *out = (int)x;
    return 1;
}

static int parse_tasks_per_node_env(int *out)
{
    const char *v = getenv("SLURM_NTASKS_PER_NODE");
    if(!v || !v[0]) return 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", v);

    for(size_t i=0; buf[i]; ++i)
    {
        if(buf[i] < '0' || buf[i] > '9') { buf[i] = '\0'; break; }
    }
    if(!buf[0]) return 0;
    *out = atoi(buf);
    return (*out > 0);
}

static int query_scontrol_job(uint32_t job_id, int *nnodes, int *ntasks, int *ntpn)
{

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "scontrol show job -d %u 2>/dev/null", job_id);

    FILE *fp = popen(cmd, "r");
    if(!fp) return 0;

    char line[4096];
    int got_any = 0;

    while(fgets(line, sizeof(line), fp))
    {
        char *p;

        p = strstr(line, "NumNodes=");
        if(p) { *nnodes = atoi(p + 9); got_any = 1; }

        p = strstr(line, "NumTasks=");
        if(p) { *ntasks = atoi(p + 9); got_any = 1; }

        p = strstr(line, "TasksPerNode=");
        if(p)
        {
            int tmp = 0;
            tmp = atoi(p + 13);
            if(tmp > 0) { *ntpn = tmp; got_any = 1; }
        }
    }

    pclose(fp);
    return got_any;
}

int dqr_slurm_query_allocation(uint32_t job_id, DQRSlurmAlloc *out)
{
    if(!out) return 0;

    out->nnodes_alloc = -1;
    out->ntasks_alloc = -1;
    out->ntasks_per_node = -1;

    int nnodes = -1, ntasks = -1, ntpn = -1;

    (void)read_int_env("SLURM_NNODES", &nnodes);
    (void)read_int_env("SLURM_NTASKS", &ntasks);
    (void)parse_tasks_per_node_env(&ntpn);

    if(job_id == 0)
    {
        int jid = -1;
        if(read_int_env("SLURM_JOB_ID", &jid) && jid > 0)
            job_id = (uint32_t)jid;
    }

    if(ntasks > 0)
    {
        out->ntasks_alloc = ntasks;
        if(nnodes > 0) out->nnodes_alloc = nnodes;
        if(ntpn > 0) out->ntasks_per_node = ntpn;
        return 1;
    }

    if(nnodes > 0 && ntpn > 0)
    {
        out->nnodes_alloc = nnodes;
        out->ntasks_per_node = ntpn;
        out->ntasks_alloc = nnodes * ntpn;
        return 1;
    }

    if(job_id > 0)
    {
        nnodes = -1; ntasks = -1; ntpn = -1;
        if(query_scontrol_job(job_id, &nnodes, &ntasks, &ntpn))
        {
            if(nnodes > 0) out->nnodes_alloc = nnodes;
            if(ntpn > 0) out->ntasks_per_node = ntpn;
            if(ntasks > 0) out->ntasks_alloc = ntasks;
            else if(nnodes > 0 && ntpn > 0) out->ntasks_alloc = nnodes * ntpn;

            return (out->ntasks_alloc > 0);
        }
    }

    return 0;
}
