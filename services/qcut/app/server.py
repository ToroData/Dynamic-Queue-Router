"""gRPC server for the QCut microservice

The module exposes the circuit-cutting pipeline as a gRPC service. Accepts a full
quantum circuit in OpenQASM format, invokes the QCut worker to perform circuit cutting,
and returns:
    - manifest_json (JSON): canonical job-level contract
    - fragments_json (JSON): fragment summary
    - partition_json (JSON): cut topology summary
    - subcircuits: serialized fragment circuits, and per-fragment metadata JSON

Authors: Ricard Santiago Raigada García
Version: 0.0.1
Date: Dicember 2025
"""
from concurrent import futures
import json
import os
import grpc

from . import qcut_pb2, qcut_pb2_grpc

from .worker import QCutWorker
from .storage import Storage, ensure_dir

def _write_artifacts(job_id, manifest, fragments_summary, partition, subs):
    jobs_root = os.getenv("QCUT_JOBS_ROOT", "/data/qcut/jobs")
    out_dir = os.path.join(jobs_root, job_id, "output")
    sub_dir = os.path.join(out_dir, "subcircuits")
    ensure_dir(out_dir)
    ensure_dir(sub_dir)

    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    with open(os.path.join(out_dir, "fragments.json"), "w", encoding="utf-8") as f:
        json.dump(fragments_summary, f, indent=2)
    with open(os.path.join(out_dir, "partition.json"), "w", encoding="utf-8") as f:
        json.dump(partition, f, indent=2)

    for s in subs:
        frag_id = s["fragment_id"]
        with open(os.path.join(sub_dir, f"{frag_id}.qasm"), "w", encoding="utf-8") as f:
            f.write(s["qasm"])
        with open(os.path.join(sub_dir, f"{frag_id}.meta.json"), "w", encoding="utf-8") as f:
            json.dump(s["meta"], f, indent=2)

    response_obj = {
        "ok": True,
        "message": "ok",
        "jobId": job_id,
        "manifestJson": json.dumps(manifest, indent=2),
        "fragmentsJson": json.dumps(fragments_summary, indent=2),
        "partitionJson": json.dumps(partition, indent=2),
        "subcircuits": [
            {
                "fragmentId": s["fragment_id"],
                "qasm": s["qasm"],
                "metaJson": json.dumps(s["meta"], indent=2),
            } for s in subs
        ],
    }
    with open(os.path.join(out_dir, "response.json"), "w", encoding="utf-8") as f:
        json.dump(response_obj, f)
    with open(os.path.join(out_dir, "DONE.ok"), "w", encoding="utf-8") as f:
        f.write("ok\n")

class QCutServicer(qcut_pb2_grpc.QCutServiceServicer):
    """gRPC servicer implementation for QCutService API

    The servicer validate and process incoming gRPC requests, invokes the QCutWorker;
    serializes returned artifacts as JSON strings.

    It is stateless, and artifacts are returned to the caller who is responsible for
    storage (e.g., writing to a shared FS in the DMR sandbox).
    """
    def __init__(self):
        """Initialize the service with a single worker instance

        The worker is currently stateless and safe to reuse across requests
        """
        self.worker = QCutWorker()

    def CutCircuit(self, request, context):
        """Execute the circuit cutting pipeline

        Recives a full quantum circuit in OpenQASM format. Applies circuit cutting,
        and returns serialized artifacts as JSON strings.

        Args:
            request (qcut_pb2.CutCircuitRequest):
                gRPC request containing:
                    - job_id: unique identifier of the cutting job
                    - circuit_qasm: full quantum circuit in OpenQASM format
                    - export_format: target subcircuit format ("qiskit" or "qibo")
                    - gate_cut: whether gate cutting is allowed
                    - qpu_max_qubits: hardware constraint received from the runtime
                    - depth_threshold_qpu: depth constraint received from the runtime
                    - observable: optional observable to be reconstructed later
            context (grpc.ServicerContext): gRPC context providing RPC metadata,
                cancellation and deadline information.

        Returns:
            qcut_pb2.CutCircuitResponse:
                Response message containing:
                - ok: boolean indicating success or failure
                - message: status or error description
                - job_id: identifier of the processed job
                - manifest_json: JSON-encoded manifest describing the cut
                - fragments_json: JSON summary of generated subcircuits
                - partition_json: JSON description of the cut topology
                - subcircuits: list of subcircuits serialized as OpenQASM
                  together with per-fragment metadata
        """
        try:
            manifest, fragments_summary, partition, subs = self.worker.cut_circuit(
                job_id=request.job_id,
                circuit_qasm=request.circuit_qasm,
                export_format=request.export_format,
                gate_cut=request.gate_cut,
                qpu_max_qubits=request.qpu_max_qubits,
                depth_threshold_qpu=request.depth_threshold_qpu,
                observable=request.observable,
                findcut_max_qubits=int(os.getenv("FINDCUT_MAX_QUBITS", 17)),
                findcut_max_cuts=int(os.getenv("FINDCUT_MAX_CUTS", 4)),
                findcut_wire_cut=os.getenv("FINDCUT_WIRE_CUT", "False").lower() == "true",
            )

            _write_artifacts(
                job_id=request.job_id,
                manifest=manifest,
                fragments_summary=fragments_summary,
                partition=partition,
                subs=subs,
            )

            jobs_root = os.getenv("QCUT_JOBS_ROOT", "/home/otras/csc/sis/dmr_mod/shared/qcut/jobs")
            out_dir = os.path.join(jobs_root, request.job_id, "output")

            resp = qcut_pb2.CutCircuitResponse(
                ok=True,
                message=f"ok (artifacts written to {out_dir})",
                job_id=request.job_id,
                manifest_json="",
                fragments_json="",
                partition_json="",
            )

            return resp

        except Exception as e:
            return qcut_pb2.CutCircuitResponse(
                ok=False,
                message=f"error: {e}",
                job_id=request.job_id,
                manifest_json="",
                fragments_json="",
                partition_json="",
            )


def serve():
    """Start the QCut gRPC service

    Environment Variables:
        QCUT_BIND (str):
            Address and port where the service will listen, in the form
            "<host>:<port>". Defaults to "0.0.0.0:50051".

    Returns:
        None
    """
    bind_addr = os.environ.get("QCUT_BIND", "0.0.0.0:50051")
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    qcut_pb2_grpc.add_QCutServiceServicer_to_server(QCutServicer(), server)
    server.add_insecure_port(bind_addr)
    server.start()
    server.wait_for_termination()


if __name__ == "__main__":
    serve()
