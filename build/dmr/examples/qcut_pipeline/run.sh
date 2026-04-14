#!/bin/bash
#SBATCH -J qcut-pipeline
#SBATCH -o output/qcut-pipeline.%j.out
#SBATCH -e output/qcut-pipeline.%j.err
#SBATCH -N 4
#SBATCH --ntasks=73
#SBATCH --ntasks-per-node=19
#SBATCH --cpus-per-task=1
#SBATCH --mem=16G
#SBATCH --partition=ilk
#SBATCH -t 06:00:00

set -euo pipefail

# logging
log() {
  local ts file line
  ts="$(date '+%Y-%m-%d %H:%M:%S.%3N %z' 2>/dev/null || date '+%Y-%m-%d %H:%M:%S %z')"
  file="${BASH_SOURCE[1]##*/}"
  line="${BASH_LINENO[0]}"
  echo "${ts} [${file}:${line}] $*"
}

declare -A __T0

tstart() {
  local name="$1"
  __T0["$name"]="$(date +%s%3N 2>/dev/null || date +%s000)"
  log "START [$name]"
}

tend() {
  local name="$1"
  local t1 t0 dt_ms dt_s
  t1="$(date +%s%3N 2>/dev/null || date +%s000)"
  t0="${__T0[$name]:-}"
  if [[ -z "$t0" ]]; then
    log "END [$name] (sin tstart previo)"
    return
  fi
  dt_ms=$((t1 - t0))
  dt_s=$(awk "BEGIN {printf \"%.3f\", ${dt_ms}/1000}")
  log "END   [$name] elapsed=${dt_s}s"
}

with_timing() {
  local name="$1"; shift
  tstart "$name"
  "$@"
  tend "$name"
}

JOB_START_MS="$(date +%s%3N 2>/dev/null || date +%s000)"

cleanup_server() {
  if [[ -n "${QCUT_SERVER_PID:-}" ]]; then
    if kill -0 "${QCUT_SERVER_PID}" 2>/dev/null; then
      log "Stopping QCut server PID=${QCUT_SERVER_PID}"
      kill "${QCUT_SERVER_PID}" 2>/dev/null || true
      wait "${QCUT_SERVER_PID}" 2>/dev/null || true
    fi
  fi
}

on_exit_wrapper() {
  local rc=$?
  local end_ms dt_ms dt_s status

  cleanup_server

  end_ms="$(date +%s%3N 2>/dev/null || date +%s000)"
  dt_ms=$((end_ms - JOB_START_MS))
  dt_s=$(awk "BEGIN {printf \"%.3f\", ${dt_ms}/1000}")

  if [[ $rc -eq 0 ]]; then
    status="SUCCESS"
  else
    status="FAIL(rc=${rc})"
  fi

  log "JOB_END status=${status} total_elapsed=${dt_s}s"
  exit "$rc"
}

trap on_exit_wrapper EXIT

tstart "setup_env"

export BASE_DIR="/home/otras/csc/sis/dmr_mod"
export QCUT_BASE_DIR="${BASE_DIR}/services/qcut"
export QCUT_VENV="${QCUT_BASE_DIR}/.venv"
export DMR_PATH="${BASE_DIR}/build/dmr"

# QCore
export QCORE_PREFIX="${BASE_DIR}/build/dmr/tools/QCore/install"

# Rutas
export PATH="${DMR_PATH}/scripts:${PATH}"
export LD_LIBRARY_PATH="${DMR_PATH}/lib:${QCORE_PREFIX}/lib64:${QCORE_PREFIX}/lib:${LD_LIBRARY_PATH:-}"

export GRPC_TIMEOUT_S=60000

# Python package path for app.server
export PYTHONPATH="${QCUT_BASE_DIR}:${PYTHONPATH:-}"

# Example working dir
EXAMPLE_DIR="${BASE_DIR}/build/dmr/examples/qcut_pipeline"
cd "${EXAMPLE_DIR}"

log "BASE_DIR=${BASE_DIR}"
log "QCUT_BASE_DIR=${QCUT_BASE_DIR}"
log "DMR_PATH=${DMR_PATH}"
log "PYTHONPATH=${PYTHONPATH}"
log "PATH=$(command -v dmr_wrapper)"
log "QCORE_PREFIX=${QCORE_PREFIX}"

if [[ ! -d "${QCUT_VENV}" ]]; then
  log "ERROR: QCUT_VENV not found at ${QCUT_VENV}"
  exit 1
fi

source "${QCUT_VENV}/bin/activate"
# module load qmio-run
export QCORE_QPU_PYTHON="${QCUT_VENV}/bin/python3"
tend "setup_env"

# QCut client + job base
QCUT_SUBMIT="${BASE_DIR}/build/qcut/client/submit.sh"
SHARED_ROOT="/mnt/netapp2/Store_uni/home/otras/csc/sis/dmr_mod/qcut/jobs"
export QCUT_JOBS_ROOT="${SHARED_ROOT}"

# QCut logs
export QCUT_LOG_DIR="${EXAMPLE_DIR}/output"
export QCUT_RUN_ID="${SLURM_JOB_ID:-$(date +%s)}"

mkdir -p "${QCUT_JOBS_ROOT}"
mkdir -p "${QCUT_LOG_DIR}"
mkdir -p "${EXAMPLE_DIR}/output"

# QASM input in example dir
LOCAL_QASM="${LOCAL_QASM:-${EXAMPLE_DIR}/circuit.qasm}"

# Create JOB_ID and SHARED_BASE
JOB_ID="job-${SLURM_JOB_ID:-$(date +%s)}"
SHARED_BASE="${SHARED_ROOT}/${JOB_ID}"

log "JOB_ID=${JOB_ID}"
log "SHARED_BASE=${SHARED_BASE}"
log "LOCAL_QASM=${LOCAL_QASM}"

export DMR_QCUT_OUTPUT_DIR="${SHARED_BASE}/output"

# --- QPU backend selector ---
# DQR_QC_BACKEND selects which QPU handles quantum (QC) fragments.
# CPU backend is always used for HPC fragments (unchanged).
#   "local"  → all QC fragments go to CESGA qmio (default)
#   "cloud"  → all QC fragments go to IBM Quantum Cloud
#   "hybrid" → qasm_version=2 → qmio, qasm_version=3 → IBM Cloud
# Extensible: add new cases for future QPU providers (e.g., "aws", "ionq").
export DQR_QC_BACKEND="${DQR_QC_BACKEND:-local}"

# IBM Quantum Cloud credentials (required when DQR_QC_BACKEND=cloud|hybrid)
export IBM_QUANTUM_TOKEN="${IBM_QUANTUM_TOKEN:-}"
export IBM_QUANTUM_CHANNEL="${IBM_QUANTUM_CHANNEL:-ibm_quantum}"
export IBM_QUANTUM_INSTANCE="${IBM_QUANTUM_INSTANCE:-ibm-q/open/main}"
export IBM_QUANTUM_BACKEND="${IBM_QUANTUM_BACKEND:-}"   # empty = least_busy()
export IBM_QPU_MAX_TTL="${IBM_QPU_MAX_TTL:-7200}"
export IBM_QPU_SESSION_FILE="${SHARED_BASE}/.ibm_session_id"

log "DQR_QC_BACKEND=${DQR_QC_BACKEND}"
if [[ "${DQR_QC_BACKEND}" != "local" ]]; then
  log "IBM_QUANTUM_BACKEND=${IBM_QUANTUM_BACKEND:-least_busy}"
  log "IBM_QPU_MAX_TTL=${IBM_QPU_MAX_TTL}"
  log "IBM_QPU_SESSION_FILE=${IBM_QPU_SESSION_FILE}"
fi

# --- stage inputs ---
tstart "stage_inputs"
mkdir -p "${SHARED_BASE}/input" "${SHARED_BASE}/output"
cp -f "${LOCAL_QASM}" "${SHARED_BASE}/input/circuit.qasm"
tend "stage_inputs"

log "Copied QASM -> ${SHARED_BASE}/input/circuit.qasm"

# --- QCut params ---
export FINDCUT_MAX_QUBITS="${FINDCUT_MAX_QUBITS:-10}"
export FINDCUT_MAX_CUTS="${FINDCUT_MAX_CUTS:-4}"
# export FINDCUT_MAX_COMPONENTS=
export FINDCUT_WIRE_CUT=False
export FINDCUT_GATE_CUT=True
# export FINDCUT_IMPLEMENTATION=qdislib
# export FINDCUT_WEIGHTS_JSON='{"JSON": "JSON"}'

TARGET="${QCUT_TARGET:-127.0.0.1:50051}"

# --- start local gRPC server ---
tstart "qcut_server_start"

export QCUT_BIND="0.0.0.0:50051"

python -m app.server & QCUT_SERVER_PID=$!

log "Started QCut server with PID=${QCUT_SERVER_PID} on ${QCUT_BIND}"

ready=0
for i in {1..20}; do
  if python - <<'PY'
import socket, sys
s = socket.socket()
s.settimeout(1.0)
try:
    s.connect(("127.0.0.1", 50051))
    sys.exit(0)
except Exception:
    sys.exit(1)
finally:
    s.close()
PY
  then
    log "QCut gRPC server is listening at attempt=${i}"
    ready=1
    break
  fi
  sleep 1
done

if [[ "${ready}" -ne 1 ]]; then
  log "QCut gRPC server did not become ready"
  log "Server stderr:"
  tail -n 50 "${EXAMPLE_DIR}/output/qcut-server.${SLURM_JOB_ID}.err" || true
  log "Server stdout:"
  tail -n 50 "${EXAMPLE_DIR}/output/qcut-server.${SLURM_JOB_ID}.out" || true
  exit 1
fi

tend "qcut_server_start"

# --- QCut submit (gRPC) ---
log "Calling QCut at TARGET=${TARGET}"
with_timing "qcut_submit" "${QCUT_SUBMIT}" "${TARGET}" "${JOB_ID}"
log "QCut artifacts written under: ${SHARED_BASE}/output"

# --- QCut limits for DMR ---
export DMR_QCUT_LABEL_MODE=autobudget
export DMR_QCUT_AUTOBUDGET_QC_PCT=0.3
export DMR_QCUT_AUTOBUDGET_HPC_PCT=0.5
export DMR_QCUT_AUTOBUDGET_UNDEF_PCT=0.2
log  "Set QCut limits for DMR."

# --- labeling ---
# Label QCut artifacts for DMR consumption
export DMR_QCUT_OUTPUT_DIR="${SHARED_BASE}/output"
with_timing "qcut_labeling" ./dmr_qcut_label_job "${JOB_ID}"
log  "Labeled QCut artifacts for DMR consumption."

log  "Label outputs:"
ls -lah "${SHARED_BASE}/output/dmr_labels.csv" || true

# --- DMR policy test ---
tstart "activate_dmr_policy_driver_setup"
export DMR_QCUT_JOB_DIR="${SHARED_BASE}/output"
export DMR_QCUT_OUTPUT_DIR="${SHARED_BASE}/output"

export DMR_QCUT_DQR_DEBUG=2

# Allow policy to move within [min,max]
export DMR_POLICY_MIN_NODES=1
export DMR_POLICY_MAX_NODES=4
export DMR_POLICY_STRIDE=2
export DMR_QCUT_BALANCE_BAND=0

export DQR_PREFER_ITER0_UNDECIDED=HPC
export DQR_PREFER_ITERN_UNDECIDED=QC
export DQR_ALLOW_FAILOVER_QC_TO_HPC=1
export DQR_ALLOW_RETRY_QC_ON_FAILURE=1
export DQR_QC_DEGRADED=0
export DQR_STRICT_QC_LABEL=0
export DQR_QC_SLOTS_TOTAL=22
export DQR_MAX_TRANSIENT_RETRIES=5

# --- Partición QPU (qmio CESGA) ---
export QMIO_SLURM_PARTITION="qpu"
export QMIO_SLURM_QOS="qpu"
export QMIO_TIMEOUT_S=7200
export QMIO_BACKEND_NAME="qpu"
export QMIO_TUNNEL_TIME_LIMIT=3600
export QMIO_RESERVATION_NAME=
export QMIO_DRY_RUN=1

export UCX_TLS=tcp,self
# module load qmio-run
# module load qmio/hpc gcc/12.3.0 qmio-tools/0.2.1-python-3.9.9
# module load qiskit/2.2.3-python-3.9.9
export QCORE_QPU_PYTHON="$(which python3)"
tend "activate_dmr_policy_driver_setup"

# --- MPI host resolution ---
tstart "mpi_host_resolution"
HOSTS_WITH_COUNTS=$(scontrol show hostnames "$SLURM_JOB_NODELIST" \
    | awk -v ntpn="$SLURM_NTASKS_PER_NODE" '{printf "%s:%d,", $1, ntpn}' \
    | sed 's/,$//')
tend "mpi_host_resolution"

log  "Launching DMR policy driver with MPI (np=${SLURM_NTASKS})"
log  "HOSTS_WITH_COUNTS=$HOSTS_WITH_COUNTS"
with_timing "dmr_policy_driver"  dmr_wrapper mpirun \
    --host "${HOSTS_WITH_COUNTS}" \
    -np "${SLURM_NTASKS}" \
    -x DMR_QCUT_JOB_DIR -x DMR_QCUT_OUTPUT_DIR \
    -x DMR_POLICY_MIN_NODES -x DMR_POLICY_MAX_NODES -x DMR_POLICY_STRIDE \
    -x DMR_QCUT_BALANCE_BAND -x DQR_ALLOW_FAILOVER_QC_TO_HPC -x DQR_QC_DEGRADED \
    -x DQR_ALLOW_RETRY_QC_ON_FAILURE -x DQR_STRICT_QC_LABEL -x DQR_QC_SLOTS_TOTAL \
    -x DQR_MAX_TRANSIENT_RETRIES -x DQR_PREFER_ITER0_UNDECIDED -x DQR_PREFER_ITERN_UNDECIDED \
    -x DMR_QCUT_DQR_DEBUG -x QMIO_DRY_RUN -x QMIO_TIMEOUT_S -x QMIO_SLURM_PARTITION \
    -x QMIO_SLURM_QOS  -x QMIO_BACKEND_NAME -x QMIO_TUNNEL_TIME_LIMIT -x QMIO_RESERVATION_NAME \
    -x QCORE_QPU_PYTHON -x PYTHONPATH -x LD_LIBRARY_PATH \
    -x DQR_QC_BACKEND \
    -x IBM_QUANTUM_TOKEN -x IBM_QUANTUM_CHANNEL -x IBM_QUANTUM_INSTANCE \
    -x IBM_QUANTUM_BACKEND -x IBM_QPU_MAX_TTL -x IBM_QPU_SESSION_FILE \
    ./qcut_policy_driver "${JOB_ID}"

log  "DMR policy driver completed."


# ─────────────────────────────────────────────────────────────────────────────
# QCut dispatch-wave visualization
# Genera qcut-pipeline.<JOBID>_waves.{pdf,png} en SHARED_BASE/output/
# ─────────────────────────────────────────────────────────────────────────────
tstart "qcut_viz"

VIZ_SCRIPT="/home/otras/csc/sis/viz/qcut_dispatch_viz.py"
VIZ_VENV="/home/otras/csc/sis/viz/venv"
ERR_LOG="${QCUT_LOG_DIR}/qcut-pipeline.${SLURM_JOB_ID}.err"
VIZ_OUT="${SHARED_BASE}/output"

if [[ ! -f "${VIZ_SCRIPT}" ]]; then
  log "WARNING: viz script not found at ${VIZ_SCRIPT} — skipping visualization"
elif [[ ! -f "${ERR_LOG}" ]]; then
  log "WARNING: .err log not found at ${ERR_LOG} — skipping visualization"
else
  log "Generating dispatch-wave figure from ${ERR_LOG}"

  if [[ -f "${VIZ_VENV}/bin/activate" ]]; then
    source "${VIZ_VENV}/bin/activate"
    log "Using viz venv: ${VIZ_VENV}"
  else
    log "viz venv not found at ${VIZ_VENV}, using current Python: $(which python)"
  fi

  python "${VIZ_SCRIPT}" "${ERR_LOG}" -o "${VIZ_OUT}/" \
    && log "Figures written to ${VIZ_OUT}/" \
    || log "WARNING: visualization script exited with error (non-fatal)"

  deactivate 2>/dev/null || true
  source "${QCUT_VENV}/bin/activate"
fi

tend "qcut_viz"
