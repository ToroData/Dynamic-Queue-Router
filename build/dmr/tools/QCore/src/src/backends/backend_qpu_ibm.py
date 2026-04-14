#!/usr/bin/env python3
from __future__ import annotations

import fcntl
import json
import os
import sys
import time
from typing import Any


def _ensure_qiskit_ibm_runtime():
    try:
        import qiskit_ibm_runtime
    except ImportError:
        import subprocess
        subprocess.check_call(
            [sys.executable, "-m", "pip", "install", "--user", "qiskit_ibm_runtime"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )

def _ibm_token() -> str:
    token = os.environ.get("IBM_QUANTUM_TOKEN", "").strip()
    if not token:
        raise RuntimeError(
            "IBM_QUANTUM_TOKEN no configurado. "
            "Exporta tu token de IBM Quantum antes de usar el backend cloud."
        )
    return token


def _ibm_channel() -> str:
    return os.environ.get("IBM_QUANTUM_CHANNEL", "ibm_quantum").strip()


def _ibm_instance() -> str | None:
    v = os.environ.get("IBM_QUANTUM_INSTANCE", "").strip()
    return v if v else None


def _ibm_backend_name() -> str | None:
    v = os.environ.get("IBM_QUANTUM_BACKEND", "").strip()
    return v if v else None


def _max_ttl() -> int:
    try:
        return max(60, int(os.environ.get("IBM_QPU_MAX_TTL", "7200")))
    except ValueError:
        return 7200


def _session_file() -> str | None:
    v = os.environ.get("IBM_QPU_SESSION_FILE", "").strip()
    return v if v else None


def _resolve_backend(service: Any, n_qubits: int) -> Any:
    name = _ibm_backend_name()
    if name:
        print(f"[IBM] Using fixed backend: {name}", file=sys.stderr, flush=True)
        return service.backend(name)
 
    print(
        f"[IBM] IBM_QUANTUM_BACKEND not defined → least_busy(min_num_qubits={n_qubits})",
        file=sys.stderr, flush=True,
    )
    backend = service.least_busy(
        operational=True,
        simulator=False,
        min_num_qubits=n_qubits,
    )
    print(f"[IBM] least_busy selected: {backend.name}", file=sys.stderr, flush=True)
    return backend
 
def _load_circuit_from_qasm(qasm_text: str):
    is_qasm3 = False
    for line in qasm_text.splitlines():
        stripped = line.strip()
        if stripped:
            if "OPENQASM 3" in stripped.upper():
                is_qasm3 = True
            break
 
    if is_qasm3:
        print(
            "[IBM] QASM 3.0 detected → qiskit.qasm3.loads",
            file=sys.stderr, flush=True,
        )
        try:
            from qiskit.qasm3 import loads as qasm3_loads
            return qasm3_loads(qasm_text)
        except ImportError:
            raise RuntimeError(
                "qiskit.qasm3 not available. "
                "Update qiskit: pip install --upgrade qiskit"
            )
        except Exception as exc3:
            # Fallback a QASM 2
            print(
                f"[IBM] qasm3 failed ({exc3}) → trying qasm2 as fallback",
                file=sys.stderr, flush=True,
            )
            try:
                from qiskit.qasm2 import loads as qasm2_loads
                return qasm2_loads(qasm_text)
            except Exception as exc2:
                raise RuntimeError(
                    f"Cannot load QASM 3.0.\n"
                    f"  qasm3 error: {exc3}\n"
                    f"  qasm2 fallback error: {exc2}"
                )
    else:
        print(
            "[IBM] QASM 2.0 detected → qiskit.qasm2.loads",
            file=sys.stderr, flush=True,
        )
        try:
            from qiskit.qasm2 import loads as qasm2_loads
            return qasm2_loads(qasm_text)
        except Exception as exc2:
            # Fallback a QASM 3
            print(
                f"[IBM] qasm2 failed ({exc2}) → trying qasm3 as fallback",
                file=sys.stderr, flush=True,
            )
            try:
                from qiskit.qasm3 import loads as qasm3_loads
                return qasm3_loads(qasm_text)
            except Exception as exc3:
                raise RuntimeError(
                    f"Cannot load QASM.\n"
                    f"  qasm2 error: {exc2}\n"
                    f"  qasm3 fallback error: {exc3}"
                )

def _run_ibm_cloud(
    qasm_text: str,
    n_qubits: int,
    shots: int,
    z_mask: int,
    meta: dict,
) -> float:

    from qiskit import QuantumCircuit
    from qiskit.transpiler.preset_passmanagers import generate_preset_pass_manager
    from qiskit_ibm_runtime import QiskitRuntimeService, SamplerV2
 
    import importlib.util
    _qulacs_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "backend_qpu_qulacs.py"
    )
    _spec = importlib.util.spec_from_file_location("backend_qpu_qulacs", _qulacs_path)
    _qulacs_mod = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_qulacs_mod)
    _ev_from_counts = _qulacs_mod._ev_from_counts
 
    service = QiskitRuntimeService(
        channel=_ibm_channel(),
        token=_ibm_token(),
        instance=_ibm_instance(),
    )
 
    backend = _resolve_backend(service, n_qubits)
    circuit = _load_circuit_from_qasm(qasm_text)
    print(
        f"[IBM] Circuito: {circuit.num_qubits}q depth={circuit.depth()}",
        file=sys.stderr, flush=True,
    )
    circuit.measure_all()
    pm = generate_preset_pass_manager(backend=backend, optimization_level=0)
    isa_circuit = pm.run(circuit)
 
    sampler = SamplerV2(mode=backend)
    job = sampler.run([isa_circuit], shots=shots)
    result = job.result()

    pub_result = result[0]
    counts = pub_result.data.meas.get_counts()
 
    return _ev_from_counts(counts, z_mask)
