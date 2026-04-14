""""QCut worker module

Implements the circuit cutting stage of the hybrid QC-HPC runtime. It receives
a full quantum circuit in OpenQASM format, applies circuit cutting techniques
using Qdislib, and produces a set of subcircuits together with structured metadata
describing the cut.

It is policy-agnostic and stateless, i.e., it does not maintain any internal state
between invocations, and does not implement any job scheduling or queuing logic.
Instead, it exposes canonical artifacts that can be consumed by an external runtime
(DMR) to make scheduling decisions.

Authors: Ricard Santiago Raigada García
Version: 0.0.2
Date: February 2026
"""
import json
import math
from typing import Any, Dict, List, Tuple
import ast
from collections.abc import Sequence
import os
from .logging_utils import append_jsonl

import Qdislib.api as qd

from qiskit import QuantumCircuit
from qiskit.qasm2 import dumps as qasm2_dumps
from qiskit.qasm3 import dumps as qasm3_dumps

import time


def _safe_count_metrics_qiskit(qc: QuantumCircuit) -> Dict[str, Any]:
    """Extract structural circuit metrics using Qiskit

    Args:
        qc (QuantumCircuit): Instance of the Qiskit quantum circuit

    Returns:
        Dict[str, Any]: Dictionary with metrics: 
            - n_qubits: number of qubits in the circuit
            - depth: circuit depth as reported by Qiskit
            - two_qubit_gates: number of two-qubit gates
    """
    depth = qc.depth()
    n_qubits = qc.num_qubits
    size_ops = int(len(getattr(qc, "data", [])))
    twoq = 0
    for inst, qargs, cargs in qc.data:
        if len(qargs) == 2:
            twoq += 1
    return {"n_qubits": n_qubits, "depth": depth, "two_qubit_gates": twoq, "size_ops": size_ops}


def _estimate_reconstruction_cost(method: str, k: int) -> int:
    """ Estimate the asymptotic classical reconstruction cost induced by circuit cutting

    Theoretical upper bound estimate derived from Qdislib paper:
        - Wire cutting: ~8^k
        - Gate cutting: ~6^k

    Args:
        method (str): Cutting method used ("GATE" or "WIRE")
        k (int): Number of cuts

    Returns:
        int: Estimated reconstruction cost factor
    """
    if k <= 0:
        return 1
    base = 6 if method.upper() == "GATE" else 8
    return int(base ** k)

def _normalize_cut_to_list(cut) -> list:
    """Normalize the cut representation into a list format for consistent downstream processing

    Args:
        cut: Cut representation which can be in various formats (str, list, tuple, etc.)

    Returns:
        list: Normalized cut representation as a list
    """
    if cut is None:
        return []

    if isinstance(cut, str):
        s = cut.strip()
        if (s.startswith("[") and s.endswith("]")) or (s.startswith("(") and s.endswith(")")):
            try:
                v = ast.literal_eval(s)
                return _normalize_cut_to_list(v)
            except Exception:
                return [s]
        return [s]

    if isinstance(cut, Sequence) and not isinstance(cut, (bytes, bytearray)):
        return list(cut)

    return [str(cut)]

def _compute_max_components(qc, max_qubits: int | None, max_components: int | None) -> int | None:
    """
    Reproduce la lógica interna de Qdislib para max_components,
    pero soportando tanto Qiskit (num_qubits) como Qibo (nqubits).
    """
    nqubits = getattr(qc, "num_qubits", getattr(qc, "nqubits", None))
    if nqubits is None:
        raise AttributeError(
            "Circuit must expose 'num_qubits' (Qiskit) or 'nqubits' (Qibo)."
        )
    if max_qubits and not max_components:
        return nqubits // int(max_qubits) + 1
    elif not max_qubits and not max_components:
        return 2
    else:
        return max_components

def _write_job_timings(job_id: str, timings: Dict[str, Any]) -> None:
    jobs_root = os.getenv("QCUT_JOBS_ROOT", "/data/qcut/jobs")
    out_dir = os.path.join(jobs_root, job_id, "output")
    os.makedirs(out_dir, exist_ok=True)

    path = os.path.join(out_dir, "timings.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(timings, f, indent=2, sort_keys=True)

class QCutWorker:
    """Stateless worker that performs circuit cutting and artifact generation
    using Qdislib as backend.
    """
    def cut_circuit(
        self,
        job_id: str,
        circuit_qasm: str,
        export_format: str,
        gate_cut: bool,
        qpu_max_qubits: int,
        depth_threshold_qpu: int,
        observable: str = "",
        findcut_max_qubits: int | None = None,
        findcut_max_cuts: int | None = None,
        findcut_max_components: int | None = None,
        findcut_wire_cut: bool | None = None,
        findcut_gate_cut: bool | None = None,
        findcut_implementation: str | None = None,
        findcut_weights_json: str | None = None,
    ) -> Tuple[Dict[str, Any], Dict[str, Any], Dict[str, Any], List[Dict[str, Any]]]:
        """Perform circuit cutting on an input quantum circuit

        The method:
          1. Parses the input OpenQASM circuit
          2. Finds a valid cut using Qdislib
          3. Generates executable subcircuits
          4. Computes structural metadata for each fragment
          5. Emits canonical JSON artifacts for downstream consumption

        Args:
            job_id (str): Unique identifier of the cutting job
            circuit_qasm (str): Full quantum circuit encoded in OpenQASM format
            export_format (str): Target subcircuit representation ("qiskit" or "qibo")
            gate_cut (bool): If True, allow gate cutting. If False, enforce wire cutting
            qpu_max_qubits (int): Hardware constraint received from the runtime
            depth_threshold_qpu (int): Depth constraint received from the runtime
            observable (str, optional): Observable to be reconstructed later. Defaults to ""

        Raises:
            TypeError: If subcircuit type is not supported for QASM export

        Returns:
            Tuple[Dict[str, Any], Dict[str, Any], Dict[str, Any], List[Dict[str, Any]]]: Tuple containing:
                - manifest : Dict[str, Any]
                    Canonical job-level description of the cut
                - fragments_summary : Dict[str, Any]
                    Summary of all generated fragments
                - partition : Dict[str, Any]
                    Description of the cut topology and number of cuts
                - subcircuits_payloads : List[Dict[str, Any]]
                    Full subcircuit payloads including QASM and metadata
        """
        qc = QuantumCircuit.from_qasm_str(circuit_qasm)

        job_t0 = time.time()
        input_metrics = _safe_count_metrics_qiskit(qc)

        append_jsonl(
            "worker_job_start",
            job_id=job_id,
            export_format=export_format,
            gate_cut=gate_cut,
            qpu_max_qubits=qpu_max_qubits,
            depth_threshold_qpu=depth_threshold_qpu,
            input_n_qubits=input_metrics["n_qubits"],
            input_depth=input_metrics["depth"],
            input_two_qubit_gates=input_metrics["two_qubit_gates"],
            input_size_ops=input_metrics["size_ops"],
        )

        if gate_cut:
            wire_cut = False if findcut_wire_cut is None else bool(findcut_wire_cut)
            gate_cut_flag = True if findcut_gate_cut is None else bool(findcut_gate_cut)
            default_max_cuts = 15
            method = "GATE"
        else:
            wire_cut = True if findcut_wire_cut is None else bool(findcut_wire_cut)
            gate_cut_flag = False if findcut_gate_cut is None else bool(findcut_gate_cut)
            default_max_cuts = 4
            method = "WIRE"

        max_cuts = default_max_cuts if findcut_max_cuts is None else int(findcut_max_cuts)
        impl = "qdislib" if not findcut_implementation else str(findcut_implementation)
    
        weights = None
        if findcut_weights_json:
            try:
                weights = json.loads(findcut_weights_json)
            except Exception:
                weights = None

        append_jsonl(
            "find_cut_start",
            job_id=job_id,
            max_qubits=findcut_max_qubits,
            max_cuts=max_cuts,
            max_components=findcut_max_components,
            wire_cut=wire_cut,
            gate_cut=gate_cut_flag,
            implementation=impl,
            weights=weights,
        )

        findcut_max_components = _compute_max_components(
            qc,
            findcut_max_qubits,
            findcut_max_components
        )

        t0 = time.time()
        cut = qd.find_cut(
            qc,
            max_qubits=findcut_max_qubits,
            max_cuts=max_cuts,
            max_components=findcut_max_components,
            wire_cut=wire_cut,
            gate_cut=gate_cut_flag,
            implementation=impl,
            weights=weights,
            verbose=False,
        )
        find_cut_s = time.time() - t0

        cut_list_debug = _normalize_cut_to_list(cut)

        append_jsonl(
            "find_cut_done",
            job_id=job_id,
            duration_s=round(find_cut_s, 6),
            k_cuts=len(cut_list_debug),
            cut=cut_list_debug,
        )

        export_format = (export_format or "qiskit").lower()

        if method == "GATE":
            k = len(cut)
            est_cost = 6 ** k

            append_jsonl(
                "subcircuits_start",
                job_id=job_id,
                method=method,
                k_cuts=k,
                estimated_reconstruction_cost=est_cost,
                export_format=export_format,
            )

            t1 = time.time()
            subcircuits = qd.gate_cutting_subcircuits(qc, cut, export_format)
            subcircuits_gen_s = time.time() - t1

            append_jsonl(
                "subcircuits_done",
                job_id=job_id,
                method=method,
                duration_s=round(subcircuits_gen_s, 6),
                n_subcircuits=len(subcircuits),
            )
        else:
            k = len(cut)
            est_cost = 8 ** k

            append_jsonl(
                "subcircuits_start",
                job_id=job_id,
                method=method,
                k_cuts=k,
                estimated_reconstruction_cost=est_cost,
                export_format=export_format,
            )

            t1 = time.time()
            subcircuits = qd.wire_cutting_subcircuits(qc, cut, export_format)
            subcircuits_gen_s = time.time() - t1

            append_jsonl(
                "subcircuits_done",
                job_id=job_id,
                method=method,
                duration_s=round(subcircuits_gen_s, 6),
                n_subcircuits=len(subcircuits),
            )

        sub_payloads: List[Dict[str, Any]] = []
        for i, sc in enumerate(subcircuits):
            frag_id = f"frag_{i:03d}"

            if isinstance(sc, QuantumCircuit):
                try:
                    sc_qasm = qasm2_dumps(sc)
                    qasm_version = 2
                except Exception:
                    sc_qasm = qasm3_dumps(sc)
                    qasm_version = 3

                meta = _safe_count_metrics_qiskit(sc)
                meta["qasm_version"] = qasm_version
            else:
                raise TypeError(f"Subcircuit type not supported for QASM export: {type(sc)}")

            meta_out = {
                "schema_version": "qcut.fragment.meta.v1",
                "job_id": job_id,
                "fragment_id": frag_id,
                "export_format": export_format,
                "qasm_version": qasm_version,
                **meta,
            }
            sub_payloads.append(
                {
                    "fragment_id": frag_id,
                    "qasm": sc_qasm,
                    "meta": meta_out,
                }
            )

        cut_list = _normalize_cut_to_list(cut)
        partition = {
            "schema_version": "qcut.partition.v1",
            "job_id": job_id,
            "method": method,
            "cut": cut_list,
            "k_cuts": int(len(cut_list)),
        }

        k = int(partition.get("k_cuts", 0))
        est_cost = _estimate_reconstruction_cost(method, k)

        fragments_summary = {
            "schema_version": "qcut.fragments_summary.v1",
            "job_id": job_id,
            "fragments": [
                {
                    "fragment_id": s["fragment_id"],
                    "n_qubits": s["meta"]["n_qubits"],
                    "depth": s["meta"]["depth"],
                    "two_qubit_gates": s["meta"]["two_qubit_gates"],
                    "size_ops": s["meta"].get("size_ops", -1),
                    "qasm_version": s["meta"]["qasm_version"],
                    "export_format": s["meta"]["export_format"],
                }
                for s in sub_payloads
            ],
        }
        manifest = {
            "schema_version": "qcut.manifest.v1",
            "job_id": job_id,
            "input": {
                "format": "OPENQASM2",
                "export_format": export_format,
                "gate_cut": gate_cut,
                "observable": observable,
            },
            "cut": {
                "method": method,
                "k_cuts": k,
                "estimated_reconstruction_cost": est_cost,
            },
            "constraints_received": {
                "qpu_max_qubits": qpu_max_qubits,
                "depth_threshold_qpu": depth_threshold_qpu,
            },
            "fragments": [
                {
                    "fragment_id": s["fragment_id"],
                    "meta": s["meta"],
                }
                for s in sub_payloads
            ],
        }

        job_total_s = time.time() - job_t0

        timings = {
            "schema_version": "qcut.timings.v1",
            "job_id": job_id,
            "input_metrics": input_metrics,
            "find_cut": {
                "max_qubits": findcut_max_qubits,
                "max_cuts": max_cuts,
                "max_components": findcut_max_components,
                "wire_cut": wire_cut,
                "gate_cut": gate_cut_flag,
                "implementation": impl,
                "weights": weights,
            },
            "result": {
                "method": method,
                "k_cuts": k,
                "estimated_reconstruction_cost": est_cost,
                "n_subcircuits": len(sub_payloads),
            },
            "timings_s": {
                "find_cut_s": round(find_cut_s, 6),
                "subcircuits_gen_s": round(subcircuits_gen_s, 6),
                "total_s": round(job_total_s, 6),
            },
        }

        _write_job_timings(job_id, timings)

        append_jsonl(
            "worker_job_done",
            job_id=job_id,
            duration_total_s=round(job_total_s, 6),
            find_cut_s=round(find_cut_s, 6),
            subcircuits_gen_s=round(subcircuits_gen_s, 6),
            method=method,
            k_cuts=k,
            estimated_reconstruction_cost=est_cost,
            n_subcircuits=len(sub_payloads),
        )
        return manifest, fragments_summary, partition, sub_payloads
