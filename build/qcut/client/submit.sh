#!/usr/bin/env bash
#
# @file submit.sh
# @brief Submit a QCut gRPC request and generate artifacts under a shared job directory.
#
# This script invokes the QCut microservice (gRPC) to cut/partition a quantum circuit provided
# in OpenQASM format, and writes a deterministic set of artifacts to a shared filesystem path
#
# The script:
#   1) Resolves the target endpoint and job identifiers
#   2) Reads the input QASM (from an explicit path or the default shared job path)
#   3) Builds a JSON request with jq
#   4) Calls qcut.QCutService/CutCircuit using grpcurl
#   5) Persists the full response and derived artifacts under:
#        ${SHARED_BASE}/output/
#        ${SHARED_BASE}/output/subcircuits/
#
# Usage:
#   submit.sh [TARGET] [JOB_ID] [QASM_PATH]
#
# Arguments:
#   TARGET    gRPC endpoint in host:port format.
#             Default: qcut-service:50051
#   JOB_ID    Logical job identifier (string). Default: job-<unix_timestamp>
#   QASM_PATH Path to a .qasm file. If omitted, uses $QASM_PATH or default shared path.
#
# Environment variables:
#   SHARED_BASE            Base directory for this job’s shared data.
#                          Default: /home/otras/csc/sis/dmr_mod/shared/qcut/jobs/${JOB_ID}
#   EXPORT_FORMAT          Export format expected by downstream tooling.
#                          Default: qiskit
#   GATE_CUT               Whether to enable gate cutting (true/false).
#                          Default: true
#   QPU_MAX_QUBITS         QPU feasibility threshold (integer).
#                          Default: 15
#   DEPTH_THRESHOLD_QPU    QPU depth threshold (integer).
#                          Default: 200
#   OBSERVABLE             Optional observable string (may be empty).
#                          Default: ""
#   QASM_PATH              Optional path override for the QASM file (if arg #3 omitted).
#
# Inputs:
#   Default QASM path (when not overridden):
#     ${SHARED_BASE}/input/circuit.qasm
#
# Outputs (written under ${SHARED_BASE}/output):
#   response.json          Full gRPC JSON response (for traceability/debug)
#   manifest.json          Response field: .manifestJson
#   fragments.json         Response field: .fragmentsJson
#   partition.json         Response field: .partitionJson
#   subcircuits/<id>.qasm  Each subcircuit QASM payload
#   subcircuits/<id>.meta.json  Each subcircuit metadata JSON
#
# Exit codes:
#   0  Success (artifacts written)
#   1  gRPC call succeeded but response ok=false OR other runtime failure
#   2  Input QASM file not found
#
set -euo pipefail

GRPCURL="/home/otras/csc/sis/dmr_mod/build/qcut/tools/bin/grpcurl"
JQ="/home/otras/csc/sis/dmr_mod/build/qcut/tools/bin/jq"
PROTO="/home/otras/csc/sis/dmr_mod/build/qcut/protos/qcut.proto"
IMPORT_PATH="/home/otras/csc/sis/dmr_mod/build/qcut/protos"

# TARGET="${1:-qcut-service:50051}"
TARGET="${1:-127.0.0.1:50051}"
JOB_ID="${2:-job-$(date +%s)}"

QCUT_JOBS_ROOT="${QCUT_JOBS_ROOT:-/home/otras/csc/sis/dmr_mod/qcut/jobs}"
SHARED_BASE="${SHARED_BASE:-${QCUT_JOBS_ROOT}/${JOB_ID}}"
OUT_DIR="${SHARED_BASE}/output"

EXPORT_FORMAT="${EXPORT_FORMAT:-qiskit}"
GATE_CUT="${GATE_CUT:-true}"
QPU_MAX_QUBITS="${QPU_MAX_QUBITS:-15}"
DEPTH_THRESHOLD_QPU="${DEPTH_THRESHOLD_QPU:-200}"
OBSERVABLE="${OBSERVABLE:-}"

DEFAULT_QASM_PATH="${SHARED_BASE}/input/circuit.qasm"
QASM_PATH="${3:-${QASM_PATH:-${DEFAULT_QASM_PATH}}}"

GRPC_TIMEOUT_S="${GRPC_TIMEOUT_S:-600}"

mkdir -p "${OUT_DIR}/subcircuits"
mkdir -p "${SHARED_BASE}/input"

if [[ ! -f "${QASM_PATH}" ]]; then
  echo "[ERROR]: QASM file not found: ${QASM_PATH}" >&2
  echo "Expected default: ${DEFAULT_QASM_PATH}" >&2
  exit 2
fi
CIRCUIT_QASM="$(cat "${QASM_PATH}")"

# Build request JSON safely
REQ_JSON="$("${JQ}" -n \
  --arg job_id "${JOB_ID}" \
  --arg circuit_qasm "${CIRCUIT_QASM}" \
  --arg export_format "${EXPORT_FORMAT}" \
  --arg observable "${OBSERVABLE}" \
  --argjson gate_cut ${GATE_CUT} \
  --argjson qpu_max_qubits ${QPU_MAX_QUBITS} \
  --argjson depth_threshold_qpu ${DEPTH_THRESHOLD_QPU} \
  '{
    job_id: $job_id,
    circuit_qasm: $circuit_qasm,
    export_format: $export_format,
    gate_cut: $gate_cut,
    qpu_max_qubits: $qpu_max_qubits,
    depth_threshold_qpu: $depth_threshold_qpu,
    observable: $observable
  }'
)"

# Call gRPC
RESP_JSON="$("${GRPCURL}" -plaintext -max-time "${GRPC_TIMEOUT_S}" \
  -import-path "${IMPORT_PATH}" \
  -proto "${PROTO}" \
  -d "${REQ_JSON}" \
  "${TARGET}" qcut.QCutService/CutCircuit
)"

DONE_FILE="${OUT_DIR}/DONE.ok"

if [[ -f "${DONE_FILE}" ]]; then
  echo "OK: server finished writing artifacts (${DONE_FILE} present)."
  rm -f "${DONE_FILE}"
else
  # Fallback validation if DONE.ok is not present
  if [[ ! -f "${OUT_DIR}/manifest.json" || ! -f "${OUT_DIR}/fragments.json" || ! -f "${OUT_DIR}/partition.json" ]]; then
    echo "[ERROR]: Expected artifacts not found in ${OUT_DIR} (server write missing?)" >&2
    echo "Check qcut-service logs and ensure QCUT_JOBS_ROOT/job_id/output matches OUT_DIR" >&2
    exit 3
  fi
fi

# Count subcircuits written by server
N_QASM="$(find "${OUT_DIR}/subcircuits" -maxdepth 1 -type f -name '*.qasm' -printf '.' | wc -c | tr -d ' ')"
N_META="$(find "${OUT_DIR}/subcircuits" -maxdepth 1 -type f -name '*.meta.json' -printf '.' | wc -c | tr -d ' ')"

if [[ "${N_QASM}" -eq 0 ]]; then
  echo "[ERROR]: No subcircuits written under ${OUT_DIR}/subcircuits" >&2
  exit 4
fi

if [[ "${N_QASM}" -ne "${N_META}" ]]; then
  echo "[ERROR]: Mismatch subcircuits files: qasm=${N_QASM} meta=${N_META}" >&2
  exit 5
fi

echo "OK: artifacts present in ${OUT_DIR}"
echo "Subcircuits: ${N_QASM}"
echo "QASM used: ${QASM_PATH}"
