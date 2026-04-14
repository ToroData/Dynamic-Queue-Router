# logging_utils.py
from __future__ import annotations

import json
import logging
import os
import socket
from datetime import datetime, timezone

LOG_DIR_DEFAULT = "/home/otras/csc/sis/dmr_mod/build/dmr/examples/qcut_pipeline/output"


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _slurm_run_id() -> str:
    job_id = os.getenv("SLURM_JOB_ID")
    array_job_id = os.getenv("SLURM_ARRAY_JOB_ID")
    array_task_id = os.getenv("SLURM_ARRAY_TASK_ID")

    if array_job_id and array_task_id:
        return f"{array_job_id}_{array_task_id}"
    if job_id:
        return job_id
    return "local"


def get_log_path(prefix: str = "qcut-pipeline", suffix: str = "jsonl") -> str:
    log_dir = os.getenv("QCUT_LOG_DIR", LOG_DIR_DEFAULT)
    os.makedirs(log_dir, exist_ok=True)
    run_id = _slurm_run_id()
    return os.path.join(log_dir, f"{prefix}.{run_id}.{suffix}")


def append_jsonl(event: str, **fields) -> None:
    payload = {
        "ts": utc_now_iso(),
        "host": socket.gethostname(),
        "pid": os.getpid(),
        "event": event,
        **fields,
    }
    path = get_log_path(prefix="qcut-pipeline", suffix="jsonl")
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(payload, ensure_ascii=False, sort_keys=True) + "\n")