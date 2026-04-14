FROM python:3.11-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Python deps
COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt

# Code + protos
COPY protos /app/protos
COPY app /app/app

# Generate gRPC stubs
RUN mkdir -p /app/app/qcutgen && touch /app/app/qcutgen/__init__.py

RUN python -m grpc_tools.protoc \
  -I /app/protos \
  --python_out=/app/app/qcutgen \
  --grpc_python_out=/app/app/qcutgen \
  /app/protos/qcut.proto

# grpc absolute import to package-relative import
RUN sed -i 's/^import qcut_pb2 as/from . import qcut_pb2 as/' \
    /app/app/qcutgen/qcut_pb2_grpc.py

# Runtime env
ENV PYTHONUNBUFFERED=1
ENV PYTHONPATH=/app

ENV QCUT_BASE_DIR=/data/qcut
ENV QCUT_BIND_ADDR=0.0.0.0:50051
ENV QCUT_MAX_WORKERS=2

EXPOSE 50051

CMD ["python", "-m", "app.server"]
