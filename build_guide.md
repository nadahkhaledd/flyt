# Flyt Build & Run Guide

> **Tested on:** Ubuntu 24.04 (WSL2), CUDA 12.6, cuDNN 9.20, Rust 1.86+  
> **GPU tested:** NVIDIA GeForce RTX 3050 Laptop (Ampere, Compute Capability 8.6)

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Pre-Build Fixes](#2-pre-build-fixes)
3. [Building Flyt](#3-building-flyt)
4. [Post-Build Verification](#4-post-build-verification)
5. [Running Flyt](#5-running-flyt)
6. [Known Limitations](#6-known-limitations)
7. [Troubleshooting](#7-troubleshooting)

---

## 1. Prerequisites

### 1.1 System Requirements

- **OS:** Ubuntu 22.04+ (native Linux recommended; WSL2 works for building but has runtime limitations — see Section 6)
- **GPU:** NVIDIA GPU with Compute Capability 7.0+ (Volta or newer). Datacenter GPUs (A40, A100, L40S, H100) are recommended for full MPS support.
- **CUDA Toolkit:** 12.x (tested with 12.6)
- **cuDNN:** 9.x (tested with 9.20)
- **Rust:** 1.85+ (older versions will fail on `edition2024` dependencies)
- **MongoDB:** 7.0+ (required by the cluster manager for VM resource configuration)

### 1.2 Install System Dependencies

```bash
# Build tools and libraries
sudo apt-get update
sudo apt-get install -y build-essential git autoconf automake libtool m4 pkg-config \
    rpcbind libssl-dev libelf-dev dos2unix rpm \
    freeglut3-dev libglu1-mesa-dev libgl1-mesa-dev libfreeimage-dev

# Install Rust via rustup (DO NOT use apt's rustc — it's too old)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
# Choose option 1 (default installation), then:
source ~/.cargo/env

# Verify Rust version is 1.85+
rustc --version
```

### 1.3 Install CUDA Toolkit (if not already installed)

For WSL2, use the `wsl-ubuntu` repository:
```bash
wget https://developer.download.nvidia.com/compute/cuda/repos/wsl-ubuntu/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install -y cuda-toolkit-12-6
```

For native Ubuntu 24.04:
```bash
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install -y cuda-toolkit-12-6
```

Add CUDA to your PATH:
```bash
echo 'export PATH=/usr/local/cuda-12.6/bin:$PATH' >> ~/.bashrc
echo 'export LD_LIBRARY_PATH=/usr/local/cuda-12.6/lib64:$LD_LIBRARY_PATH' >> ~/.bashrc
source ~/.bashrc
nvcc --version  # Should show CUDA 12.6
```

### 1.4 Install cuDNN 9.x

If your apt sources don't include cuDNN, add the full Ubuntu 24.04 NVIDIA repo:
```bash
echo "deb [signed-by=/usr/share/keyrings/cuda-archive-keyring.gpg] https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /" | sudo tee /etc/apt/sources.list.d/cuda-ubuntu2404.list
sudo apt-get update
sudo apt-get install -y libcudnn9-dev-cuda-12
```

### 1.5 Install MongoDB

```bash
curl -fsSL https://www.mongodb.org/static/pgp/server-7.0.asc | sudo gpg --dearmor -o /usr/share/keyrings/mongodb-server-7.0.gpg
echo "deb [ signed-by=/usr/share/keyrings/mongodb-server-7.0.gpg ] https://repo.mongodb.org/apt/ubuntu jammy/mongodb-org/7.0 multiverse" | sudo tee /etc/apt/sources.list.d/mongodb-org-7.0.list
sudo apt-get update
sudo apt-get install -y mongodb-org
```

### 1.6 Verify GCC (IMPORTANT)

Make sure you are using the **system GCC**, not a custom toolchain. Run:
```bash
which gcc
```

If the output is anything other than `/usr/bin/gcc` (e.g., `/u/sw/toolchains/...` or similar), you have a custom GCC in your PATH that will cause build failures because it won't search `/usr/include` for headers. Fix it for the current session:
```bash
export PATH=/usr/bin:$PATH
which gcc  # Should now show /usr/bin/gcc
```

To make this permanent, find and comment out the line in `~/.bashrc` or `~/.profile` that adds the custom toolchain to your PATH.

---

## 2. Pre-Build Fixes

### 2.1 Clone and Initialize

```bash
git clone https://github.com/cloudarxiv/flyt.git
cd flyt
git submodule update --init --recursive
```

### 2.2 Fix CRLF Line Endings

The repository may contain Windows-style line endings (CRLF) that corrupt shell scripts and Makefiles on Linux. Convert all source files to Unix line endings:

```bash
# Fix the corrupted m4 directory name in libtirpc (if it has \r appended)
cd submodules/libtirpc
if [ -d $'m4\r' ]; then mv $'m4\r' m4; fi
cd ../..

# Convert all text source files to Unix line endings
find . -path ./.git -prune -o -type f \( \
    -name "*.c" -o -name "*.h" -o -name "*.rs" -o -name "Makefile" \
    -o -name "*.sh" -o -name "*.toml" -o -name "*.x" -o -name "*.am" \
    -o -name "*.ac" -o -name "*.in" -o -name "*.py" \
\) -print -exec dos2unix {} \;

# Prevent git from re-introducing CRLF on future pulls
git config core.autocrlf input
```

### 2.3 Fix Hardcoded Config Paths

The file `control-managers/src/common/config.rs` contains hardcoded paths that must point to your system. Edit it:

```bash
nano control-managers/src/common/config.rs
```

Replace the contents with (substituting your actual username):
```rust
pub const CLMGR_CONFIG_PATH: &str = "/home/YOUR_USERNAME/configs/client-mgr.toml";
pub const RMGR_CONFIG_PATH: &str = "/home/YOUR_USERNAME/configs/cluster-mgr-config.toml";
pub const SNODE_CONFIG_PATH: &str = "/home/YOUR_USERNAME/configs/servnode-config.toml";
```

### 2.4 Set Up Configuration Files

```bash
# Create config directory and copy config files
mkdir -p ~/configs
cp cluster-mgr-config.toml ~/configs/
cp control-managers/client-mgr.toml ~/configs/
cp control-managers/servnode-config.toml ~/configs/

# Update IP addresses to localhost (for single-machine development)
sed -i 's/192.165.32.54/127.0.0.1/g' ~/configs/client-mgr.toml
sed -i 's/192.165.32.54/127.0.0.1/g' ~/configs/servnode-config.toml

# Update the cluster manager config with MongoDB connection
nano ~/configs/cluster-mgr-config.toml
```

Set the `[vm-resource-db]` section to:
```toml
[vm-resource-db]
host = "localhost"
port = 27017
user = ""
password = ""
dbname = "flyt"
```

### 2.5 Fix cuDNN 9.x API Compatibility

cuDNN 9.x removed the `cudnnAlgorithmDescriptor_t`, `cudnnAlgorithmPerformance_t`, and `cudnnAlgorithm_t` types that Flyt's code references. Wrap the affected lines in a preprocessor guard.

In `cpu/cpu-client-cudnn.c`, find the block starting at approximately line 1447 (the line with `cudnnCreateAlgorithmDescriptor`) and ending at approximately line 1458 (the line with `cudnnRestoreAlgorithm`). Add a guard before and after this block:

```c
// Add this line BEFORE the cudnnCreateAlgorithmDescriptor line:
#if CUDNN_MAJOR < 9  /* Algorithm descriptor APIs were removed in cuDNN 9 */

// ... (the 12 DEF_FN lines with cudnnAlgorithm* types) ...

// Add this line AFTER the cudnnRestoreAlgorithm line:
#endif /* CUDNN_MAJOR < 9 */
```

### 2.6 WSL2-Specific: Create CUDA Driver Symlinks

WSL2 keeps the CUDA driver library in a non-standard location. Create symlinks so the linker can find it:

```bash
sudo ln -sf /usr/lib/wsl/lib/libcuda.so /usr/local/cuda/lib64/libcuda.so
sudo ln -sf /usr/lib/wsl/lib/libcuda.so.1 /usr/local/cuda/lib64/libcuda.so.1
```

Also add the WSL NVIDIA library path to your environment:
```bash
echo 'export PATH=/usr/lib/wsl/lib:$PATH' >> ~/.bashrc
source ~/.bashrc
```

---

## 3. Building Flyt

From the repository root:

```bash
cd ~/flyt
LOG=INFO make
```

This builds everything in order: libtirpc (RPC library) → Cricket CPU server (`cricket-rpc-server`) → Cricket CPU client (`cricket-client.so`) → Rust control managers (flyt-cluster-manager, flyt-node-manager, flyt-client-manager, flytctl, flytctlnet) → Test applications.

The Rust compilation downloads dependencies on first build and takes 2-3 minutes. Subsequent builds are fast (~1 second).

**Expected warnings (safe to ignore):**
- `deprecated` warnings for `cudaDeviceGetSharedMemConfig`, `cuDeviceGetProperties`, `cuDeviceComputeCapability` — these are CUDA APIs deprecated in newer versions but still functional.
- Rust warnings about `mismatched_lifetime_syntaxes` and `const_item_interior_mutations` — code style warnings, not errors.

---

## 4. Post-Build Verification

Verify all binaries were produced:

```bash
ls -la ~/flyt/bin/
```

You should see these files:

| Binary | Description |
|--------|-------------|
| `cricket-rpc-server` | GPU virtualization server — executes CUDA API calls on behalf of clients |
| `cricket-client.so` | Client interception library — intercepts CUDA calls from applications |
| `cricket-server.so` | Shared library version of the server (for tests) |
| `flyt-cluster-manager` | Central orchestrator — scheduling, scaling, migration decisions |
| `flyt-node-manager` | GPU node daemon — reports telemetry, spawns virtualization servers |
| `flyt-client-manager` | VM-side daemon — relays app metrics, applies control commands |
| `flytctl` | CLI tool for interacting with the Flyt cluster |
| `flytctlnet` | Network CLI tool for Flyt cluster |
| `libtirpc.so` / `libtirpc.so.3` | RPC library used by Cricket |

---

## 5. Running Flyt

### 5.1 Prerequisites for Running

Flyt's GPU sharing mechanism requires **NVIDIA MPS (Multi-Process Service)** with per-context SM partitioning. This requires:

- Native Linux (not WSL2 — the MPS daemon `nvidia-cuda-mps-control` is not included in the WSL2 driver package)
- NVIDIA GPU with Volta architecture or newer (Compute Capability 7.0+)
- Datacenter GPUs (A40, A100, L40S, H100) are recommended for full MPS support

### 5.2 Starting MPS (on native Linux only)

```bash
export CUDA_VISIBLE_DEVICES=0
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nvidia-mps
export CUDA_MPS_LOG_DIRECTORY=/tmp/nvidia-mps-log
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY

# Start MPS control daemon
nvidia-cuda-mps-control -d

# Enable per-context SM partitioning (required by Flyt)
export CUDA_MPS_ENABLE_PER_CTX_DEVICE_MULTIPROCESSOR_PARTITIONING=1
```

### 5.3 Starting the Cricket Server

```bash
cd ~/flyt
LD_LIBRARY_PATH=bin:$LD_LIBRARY_PATH bin/cricket-rpc-server <rpc_version> <gpu_id> <num_sm_cores> <memory_mb>
```

**Parameters:**
- `rpc_version`: A unique integer identifier for this server instance (e.g., `1`)
- `gpu_id`: The GPU device ID to use (e.g., `0` for the first GPU)
- `num_sm_cores`: Number of SM cores to allocate to this server (e.g., `8` for half of a 16-SM GPU)
- `memory_mb`: GPU memory limit in MB (e.g., `4096` for 4GB)

**Example:**
```bash
LD_LIBRARY_PATH=bin:$LD_LIBRARY_PATH bin/cricket-rpc-server 1 0 8 4096
```

If successful, you should see:
```
welcome to cricket!
+... DEBUG: using prog=99, vers=1
+... INFO:  using TCP...
+... INFO:  listening on port XXXXX
```

### 5.4 Running a Test Application Through Flyt

In a separate terminal, run a CUDA test application with the Cricket client library injected:

```bash
cd ~/flyt
LD_LIBRARY_PATH=bin:$LD_LIBRARY_PATH LD_PRELOAD=bin/cricket-client.so tests/bin/cricket.testapp
```

The test application (matrix multiplication) should produce output showing execution timing, with all CUDA calls transparently going through Flyt's virtualization layer.

### 5.5 Running the Full Flyt Orchestration Stack

For the complete distributed system, start the components in this order:

1. **Start MongoDB** (if not already running):
   ```bash
   sudo mongod --dbpath /var/lib/mongodb --fork --logpath /var/log/mongodb.log
   ```

2. **Start the Cluster Manager** (on any machine):
   ```bash
   ./bin/flyt-cluster-manager
   ```

3. **Start the Node Manager** (on each GPU node, after enabling MPS):
   ```bash
   ./bin/flyt-node-manager
   ```

4. **Start the Client Manager** (inside each VM):
   ```bash
   ./bin/flyt-client-manager
   ```

5. **Use flytctl** to monitor and control:
   ```bash
   ./bin/flytctl --help
   ```

---

## 6. Known Limitations

### WSL2 Limitations
- The `nvidia-cuda-mps-control` daemon is **not included** in the WSL2 NVIDIA driver package. This means Flyt's SM partitioning (which relies on MPS per-context execution affinity) cannot run on WSL2. Building and compiling works perfectly on WSL2, but runtime testing requires native Linux.
- GPU compute mode cannot be changed on WSL2 (it's fixed to `Default` mode by the Windows host driver).

### Consumer GPU Limitations
- GeForce GPUs (RTX 3050, 4050, etc.) may have limited MPS support compared to datacenter GPUs (A40, A100, L40S, H100). The per-context SM partitioning feature is primarily designed for and tested on datacenter hardware.

### cuDNN 9.x Compatibility
- cuDNN 9.x removed several Algorithm Descriptor APIs. The preprocessor guard fix in Section 2.5 handles this, but if you encounter similar issues with other cuDNN types, the same `#if CUDNN_MAJOR < 9` pattern can be applied.

---

## 7. Troubleshooting

### `libelf.h: No such file or directory`
You are likely using a non-system GCC. Run `which gcc` — if it's not `/usr/bin/gcc`, add `/usr/bin` to the front of your PATH: `export PATH=/usr/bin:$PATH`

### `aclocal: error: couldn't open directory 'm4'`
The libtirpc submodule has CRLF line-ending corruption. Follow Section 2.2 to fix line endings and the m4 directory name.

### `feature 'edition2024' is required` (Rust build)
Your Rust version is too old. Install Rust via `rustup` (not `apt`). See Section 1.2.

### `Failed to create new context: the provided execution affinity is not supported`
MPS is not running or per-context SM partitioning is not enabled. Ensure you've started MPS and set `CUDA_MPS_ENABLE_PER_CTX_DEVICE_MULTIPROCESSOR_PARTITIONING=1`. See Section 5.2.

### `cannot find -lcuda`
On WSL2, create symlinks for the CUDA driver library. See Section 2.6.

### cuDNN `unknown type name 'cudnnAlgorithmDescriptor_t'`
You have cuDNN 9.x installed. Apply the preprocessor guard fix in Section 2.5.