#!/bin/bash
set -euo pipefail

ROOT="/home/otras/csc/sis/dmr_mod/build/qcut/tools"
BIN="${ROOT}/bin"
TMP="${ROOT}/tmp"
mkdir -p "${BIN}" "${TMP}"

VER="1.9.2"
TAR="grpcurl_${VER}_linux_x86_64.tar.gz"
URL="https://github.com/fullstorydev/grpcurl/releases/download/v${VER}/${TAR}"

echo "[install-grpcurl] downloading ${URL}"
curl -fL "${URL}" -o "${TMP}/${TAR}"

echo "[install-grpcurl] extracting"
tar -xzf "${TMP}/${TAR}" -C "${TMP}"

if [[ ! -f "${TMP}/grpcurl" ]]; then
  echo "ERROR: grpcurl binary not found in extracted archive."
  ls -lah "${TMP}"
  exit 1
fi

install -m 0755 "${TMP}/grpcurl" "${BIN}/grpcurl"

echo "[install-grpcurl] installed: ${BIN}/grpcurl"
"${BIN}/grpcurl" --version

JQ_VER="1.8.1"
JQ_BIN="jq-linux-amd64"
JQ_URL="https://github.com/jqlang/jq/releases/download/jq-${JQ_VER}/${JQ_BIN}"

echo "[install-jq] downloading ${JQ_URL}"
curl -fL "${JQ_URL}" -o "${TMP}/jq"

install -m 0755 "${TMP}/jq" "${BIN}/jq"
"${BIN}/jq" --version

echo "[install] tools installed in ${BIN}"
