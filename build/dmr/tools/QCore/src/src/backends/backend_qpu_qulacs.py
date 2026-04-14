#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import sys
import time
from typing import Any

def _ensure_qasm3_import():
    try:
        import qiskit_qasm3_import
    except ImportError:
        import subprocess
        subprocess.check_call(
            [sys.executable, "-m", "pip", "install", "--user", "qiskit_qasm3_import"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )


def _shots() -> int:
    try:
        s = int(os.environ.get("QCORE_SHOTS", "1024"))
        return s if s > 0 else 1024
    except ValueError:
        return 1024


def _dry_run() -> bool:
    return os.environ.get("QMIO_DRY_RUN", "0").strip() == "1"


def _backend_name() -> str:
    return os.environ.get("QMIO_BACKEND_NAME", "qpu")


def _tunnel_time_limit() -> str | None:
    val = os.environ.get("QMIO_TUNNEL_TIME_LIMIT", "").strip()
    return val if val else None


def _reservation_name() -> str | None:
    val = os.environ.get("QMIO_RESERVATION_NAME", "").strip()
    return val if val else None


def _gate_cut_term_sign(index: int) -> int:
    number, change_sign = index, False
    while number:
        if number % 6 in (3, 5):
            change_sign = not change_sign
        number //= 6
    return -1 if change_sign else 1


def _build_pauli_and_mask(meta: dict, n_qubits: int) -> tuple[str, int]:
    pauli = ["Z"] * n_qubits
    for idx in meta.get("obs_i_qubits", []):
        if isinstance(idx, int) and 0 <= idx < n_qubits:
            pauli[idx] = "I"
    z_mask = sum(1 << q for q in range(n_qubits) if pauli[q] == "Z")
    return "".join(pauli), z_mask


def _ev_from_counts(counts: dict[str, int], z_mask: int) -> float:
    total = sum(counts.values())
    if total == 0:
        return 0.0
    acc = sum(
        cnt * ((-1) if bin(int(bs.strip(), 2) & z_mask).count("1") % 2 else 1)
        for bs, cnt in counts.items()
        if bs.strip()
    )
    return acc / total


def _build_circuit_from_qasm(qasm_text: str):
    try:
        from qiskit.qasm2 import loads as qasm2_loads
        return qasm2_loads(qasm_text)
    except Exception:
        _ensure_qasm3_import()
        from qiskit.qasm3 import loads as qasm3_loads
        return qasm3_loads(qasm_text)


def _run_dry(qasm_text: str, n_qubits: int, shots: int, z_mask: int) -> float:
    from qiskit.quantum_info import Statevector

    circuit = _build_circuit_from_qasm(qasm_text)
    circuit_nm = circuit.remove_final_measurements(inplace=False)
    sv = Statevector.from_instruction(circuit_nm)
    counts = sv.sample_counts(shots)
    return _ev_from_counts(counts, z_mask)


def _run_qmio(qasm_text: str, n_qubits: int, shots: int, z_mask: int) -> float:
    from qmiotools.integrations.qiskitqmio import QmioBackend
    from qiskit import transpile

    circuit = _build_circuit_from_qasm(qasm_text)
    backend = QmioBackend()
    circuit_t = transpile(circuit, backend, optimization_level=2)
    job = backend.run(circuit_t, shots=shots)
    counts = job.result().get_counts()
    return _ev_from_counts(counts, z_mask)

def _load_manifest(meta_path: str) -> dict:
    directory = os.path.dirname(os.path.abspath(meta_path))
    for _ in range(4):
        candidate = os.path.join(directory, "manifest.json")
        if os.path.isfile(candidate):
            try:
                with open(candidate, encoding="utf-8") as f:
                    return json.load(f)
            except Exception:
                return {}
        parent = os.path.dirname(directory)
        if parent == directory:
            break
        directory = parent
    return {}


def _use_ibm_cloud(meta: dict) -> bool:
    qc_backend = os.environ.get("DQR_QC_BACKEND", "local").strip().lower()
    if qc_backend == "cloud":
        return True
    if qc_backend == "hybrid":
        qasm_version = meta.get("qasm_version", 2)
        return int(qasm_version) == 3
    return False
 
 
def run(meta_path: str, qasm_path: str) -> dict[str, Any]:
    t_start = time.monotonic()
 
    with open(meta_path, encoding="utf-8") as f:
        meta: dict = json.load(f)
 
    job_id      = meta.get("job_id", "")
    fragment_id = meta.get("fragment_id", "")
    n_qubits: int = int(meta.get("n_qubits", 1))
    shots = _shots()

    manifest = _load_manifest(meta_path)
    manifest_cut = manifest.get("cut", {})
    is_gate_cut = str(manifest_cut.get("method", "")).upper() == "GATE"
 
    gate_cut_index, gate_cut_sign = -1, 1
    if is_gate_cut:
        for key in ("gate_cut_index", "term_index", "index"):
            if isinstance(meta.get(key), int):
                gate_cut_index = meta[key]
                gate_cut_sign  = _gate_cut_term_sign(gate_cut_index)
                break

    pauli, z_mask = _build_pauli_and_mask(meta, n_qubits)
 
    with open(qasm_path, encoding="utf-8") as f:
        qasm_text = f.read()

    use_cloud = _use_ibm_cloud(meta)
    t_sim = time.monotonic()

    if use_cloud:
        import importlib.util
        _ibm_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "backend_qpu_ibm.py")
        _spec = importlib.util.spec_from_file_location("backend_qpu_ibm", _ibm_path)
        _mod = importlib.util.module_from_spec(_spec)
        _spec.loader.exec_module(_mod)
        ev = _mod._run_ibm_cloud(qasm_text, n_qubits, shots, z_mask, meta)
        ibm_name = _mod._ibm_backend_name() or "least_busy"
        backend_selected = f"ibm_{ibm_name}"
        backend_device   = "qpu_cloud"
    elif _dry_run():
        ev = _run_dry(qasm_text, n_qubits, shots, z_mask)
        backend_selected = "statevector_dry_run"
        backend_device   = "local"
    else:
        ev = _run_qmio(qasm_text, n_qubits, shots, z_mask)
        backend_selected = "qmio_qpu"
        backend_device   = "qpu"
 
    simulate_ms    = (time.monotonic() - t_sim)   * 1000.0
    observables_ms = 0.0  # included in simulate_ms for the QPU
    total_ms       = (time.monotonic() - t_start) * 1000.0
 
    backend_selected = "qpu_ibm" if use_cloud else "qpu_qulacs"
    backend_device   = "qpu_cloud" if use_cloud else "qpu"
 
    cut: dict[str, Any] = {"type": "gate" if is_gate_cut else "wire"}
    if is_gate_cut and gate_cut_index >= 0:
        cut["term_index"] = gate_cut_index
        cut["term_sign"]  = gate_cut_sign
 
    return {
        "schema_version": "qcore.result.v1",
        "status": "ok",
        "job_id": job_id,
        "fragment_id": fragment_id,
        "backend": {
            "selected": backend_selected,
            "device": backend_device,
            "gpus": 0,
        },
        "timing_ms": {
            "parse": 0.0,
            "simulate": simulate_ms,
            "observables": 0.0,
            "total": total_ms,
        },
        "results": {
            "expected_value": ev,
            "shots": shots,
            "pauli": pauli,
        },
        "cut": cut,
        "diagnostics": {"warnings": [], "info": []},
    }

def _error_result(meta_path: str, msg: str) -> dict[str, Any]:
    meta: dict = {}
    try:
        with open(meta_path, encoding="utf-8") as f:
            meta = json.load(f)
    except Exception:
        pass
    return {
        "schema_version": "qcore.result.v1",
        "status": "error",
        "job_id": meta.get("job_id", ""),
        "fragment_id": meta.get("fragment_id", ""),
        "error": {"code": "QCORE_EBACKEND", "message": msg, "details": {}},
        "diagnostics": {"warnings": [], "info": []},
    }


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(json.dumps({
            "schema_version": "qcore.result.v1",
            "status": "error",
            "job_id": "", "fragment_id": "",
            "error": {
                "code": "QCORE_EINVALID",
                "message": f"Uso: {sys.argv[0]} <meta_json_path> <qasm_path>",
                "details": {},
            },
            "diagnostics": {"warnings": [], "info": []},
        }, indent=2))
        sys.exit(1)

    try:
        print(json.dumps(run(sys.argv[1], sys.argv[2]), indent=2))
    except Exception as exc:
        print(json.dumps(_error_result(sys.argv[1], str(exc)), indent=2))
        sys.exit(1)