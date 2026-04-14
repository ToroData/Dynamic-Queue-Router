# QCore

Ensure proper and needed installation of  NVCC and cuQuantum library. Check running `which nvcc` and `dpkg -s libcuquantum0-dev-cuda-12`. 

```bash
sudo apt update
# Cuda toolkit
sudo apt install -y nvidia-cuda-toolkit

# Cuda Keyring and cuquantum lib
source /etc/os-release
ver="${VERSION_ID//./}"
repo="ubuntu${ver}"
sudo apt-get update
sudo apt-get install -y wget ca-certificates
wget "https://developer.download.nvidia.com/compute/cuda/repos/${repo}/x86_64/cuda-keyring_1.1-1_all.deb"
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install -y libcuquantum0-dev-cuda-12
```

Run:

```bash
rm -rf build/ install/

module load qmio/hpc gcc/12.3.0 boost/1.85.0
cd build/dmr/tools/QCore
mkdir -p build
mkdir -p install

module load qmio/hpc gcc/12.3.0 boost/1.85.0
cmake -S src -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/install" \
  -DQCORE_ENABLE_QULACS=ON \
  -DQCORE_FETCH_QULACS=ON \
  -DQCORE_ENABLE_CUQUANTUM=OFF \
  -DCUDAToolkit_ROOT=/usr

cmake --build build -j
cmake --install build

```

Compile `run_qcore_batch.c` using:

```bash
gcc -O2 -std=c11 -I./src/include \
  -o run_qcore_batch run_qcore_batch.c \
  -L./build -lqcore -lqtensor -lstdc++ -lm \
  -Wl,-rpath,'$ORIGIN/build'
```

Run the program:

```bash
QCORE_NUM_CUTS= CUDA_VISIBLE_DEVICES= QCORE_SHOTS=1024 \
./run_qcore_batch /mnt/c/Users/usuario/OneDrive/Documentos/QCore/examples/job-28/output \
  examples/job-28/qcore_outputs_job-28.jsonl
```
