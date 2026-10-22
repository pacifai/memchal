#!/usr/bin/env bash
# gpu_pod_setup.sh — the first command to run on a rented GPU box.
#
# Three jobs, in this order, because each one can make the next pointless:
#
#   1. Describe the machine, and record it. A trial without its machine record
#      is not a measurement, it is a number. This writes results/machine.json
#      next to the trial output.
#   2. Refuse boxes whose results would be meaningless — no GPU, MIG slices
#      instead of a whole card, or a compute capability this build cannot target.
#   3. Build the GPU targets and run the known-answer gate, which is what says
#      the CUDA kernel computes the same challenge as the CPU reference.
#
# It answers the RunPod-specific unknowns in the first few seconds rather than
# after an hour of rental: whether clocks can be locked, and whether RDMA
# devices exist at all.
#
# Usage:  bash scripts/gpu_pod_setup.sh [--device N] [--skip-build]
# Exit:   0 everything passed, 1 the box is unusable, 2 the build or gate failed.
set -uo pipefail
cd "$(dirname "$0")/.."

DEVICE=0
SKIP_BUILD=0
while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

mkdir -p results
hr() { printf '%s\n' "------------------------------------------------------------"; }
say() { printf '\n== %s ==\n' "$1"; }

fatal() { printf '\nFATAL: %s\n' "$1" >&2; exit 1; }

# --- 1. is there a GPU at all ------------------------------------------------
say "GPU"
command -v nvidia-smi >/dev/null 2>&1 || fatal "nvidia-smi not found — no driver visible in this container"
nvidia-smi --query-gpu=index,name,compute_cap,memory.total,driver_version,ecc.mode.current \
           --format=csv,noheader || fatal "nvidia-smi failed"

GPU_NAME=$(nvidia-smi -i "$DEVICE" --query-gpu=name --format=csv,noheader 2>/dev/null)
GPU_CC=$(nvidia-smi -i "$DEVICE" --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | tr -d '.')
GPU_MEM=$(nvidia-smi -i "$DEVICE" --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null)
DRIVER=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1)
GPU_COUNT=$(nvidia-smi --query-gpu=index --format=csv,noheader 2>/dev/null | wc -l | tr -d ' ')
[ -n "$GPU_NAME" ] || fatal "device $DEVICE not present"

# The Makefile ships cubins for 80/86/90 and a compute_75 PTX floor. Below 75
# there is no fallback and the binary will not load.
if [ -n "$GPU_CC" ] && [ "$GPU_CC" -lt 75 ] 2>/dev/null; then
  fatal "compute capability $GPU_CC is below the compute_75 PTX floor in the Makefile"
fi

# --- 2. is it a WHOLE card ---------------------------------------------------
say "exclusivity"
MIG=$(nvidia-smi -i "$DEVICE" --query-gpu=mig.mode.current --format=csv,noheader 2>/dev/null || echo "N/A")
echo "MIG mode:        $MIG"
case "$MIG" in
  Enabled*) fatal "MIG is enabled — you have a slice, not a card; bandwidth numbers would be meaningless" ;;
esac

COMPUTE_MODE=$(nvidia-smi -i "$DEVICE" --query-gpu=compute_mode --format=csv,noheader 2>/dev/null)
echo "compute mode:    $COMPUTE_MODE"

# Other tenants on the card show up as processes you did not start.
OTHER=$(nvidia-smi -i "$DEVICE" --query-compute-apps=pid,used_memory --format=csv,noheader 2>/dev/null)
if [ -n "$OTHER" ]; then
  echo "WARNING: other processes already hold memory on this GPU:"
  echo "$OTHER" | sed 's/^/  /'
else
  echo "other tenants:   none visible"
fi

# vCPU per GPU infers whether you rented the whole node. Fractional rentals
# share host DRAM and the PCIe root complex invisibly, which does not hurt HBM
# numbers but does invalidate any host-DRAM or zero-copy adversary row.
VCPU=$(nproc 2>/dev/null || echo "?")
echo "vCPUs:           $VCPU  (for $GPU_COUNT visible GPU(s))"
if [ "$GPU_COUNT" = "1" ]; then
  echo "  note: a single-GPU rental shares host DRAM and PCIe with other tenants."
  echo "        Fine for HBM-resident work; do not report host-DRAM or zero-copy"
  echo "        adversary numbers from this box."
fi

# --- 3. the timing instrument ------------------------------------------------
say "clocks and throttling"
nvidia-smi -i "$DEVICE" --query-gpu=clocks.sm,clocks.mem,clocks.max.sm,temperature.gpu,power.draw,power.limit \
           --format=csv,noheader 2>/dev/null
echo -n "clock locking:   "
if nvidia-smi -i "$DEVICE" -lgc 0,0 >/dev/null 2>&1; then
  nvidia-smi -i "$DEVICE" -rgc >/dev/null 2>&1
  echo "PERMITTED (lock clocks before timed runs)"
  CLOCK_LOCK="permitted"
else
  echo "DENIED (expected on a container provider)"
  echo "  Treat DVFS as a measured variance term, not noise: warm up 30s+ before"
  echo "  the first recorded trial, record clocks.sm with EVERY trial, report"
  echo "  percentiles rather than means, and interleave honest and adversary"
  echo "  trials instead of running them in blocks."
  CLOCK_LOCK="denied"
fi
echo "throttle reasons:"
nvidia-smi -i "$DEVICE" -q -d PERFORMANCE 2>/dev/null | grep -A10 -i "clocks event reasons\|clocks throttle reasons" | sed 's/^/  /' | head -14

# --- 4. topology and fabric --------------------------------------------------
say "topology"
nvidia-smi topo -m 2>/dev/null | head -20 || echo "topo unavailable"
PCIE_GEN=$(nvidia-smi -i "$DEVICE" --query-gpu=pcie.link.gen.current,pcie.link.width.current --format=csv,noheader 2>/dev/null)
echo "PCIe link:       $PCIE_GEN"
if nvidia-smi topo -m 2>/dev/null | grep -qE '\bNV[0-9]+\b'; then
  echo "NVLink:          present between GPUs (peer-residency adversary is testable here)"
  HAS_NVLINK="yes"
else
  echo "NVLink:          NOT present (single card or PCIe-only — peer adversary not testable)"
  HAS_NVLINK="no"
fi

# The 60-second answer to whether the RDMA experiments can run here at all.
say "RDMA (settles whether the A1/A2b rows are possible on this provider)"
if [ -d /dev/infiniband ] && [ -n "$(ls -A /dev/infiniband 2>/dev/null)" ]; then
  echo "/dev/infiniband: present"
  ls /dev/infiniband | sed 's/^/  /'
  command -v ibv_devinfo >/dev/null 2>&1 && ibv_devinfo 2>/dev/null | head -12 | sed 's/^/  /'
  HAS_RDMA="yes"
else
  echo "/dev/infiniband: ABSENT — RDMA experiments cannot run on this box."
  echo "  This is the expected RunPod answer. It does not block the GPU port;"
  echo "  it re-scopes the ODP/RDMA adversary rows to another provider."
  HAS_RDMA="no"
fi
lsmod 2>/dev/null | grep -q nvidia_peermem && echo "nvidia_peermem:  loaded" || echo "nvidia_peermem:  not loaded"

# --- 5. record the machine ---------------------------------------------------
STAMP=$(date -u +%Y-%m-%dT%H:%M:%SZ)
SESSION=$(date -u +%Y%m%dT%H%M%SZ)-$(hostname | tr -cd '[:alnum:]' | tail -c 8)
cat > results/machine.json <<JSON
{
  "session": "$SESSION",
  "recorded_utc": "$STAMP",
  "gpu": "$GPU_NAME",
  "compute_cap": "$GPU_CC",
  "gpu_memory_mib": "$GPU_MEM",
  "gpu_count": "$GPU_COUNT",
  "driver": "$DRIVER",
  "vcpus": "$VCPU",
  "mig_mode": "$MIG",
  "clock_lock": "$CLOCK_LOCK",
  "nvlink": "$HAS_NVLINK",
  "rdma": "$HAS_RDMA",
  "pcie": "$PCIE_GEN"
}
JSON
say "machine record"
cat results/machine.json
echo
echo "Every trial from this box must carry this record. Absolute microseconds"
echo "are not comparable to any other session; only within-session ratios are."

# --- 6. build and gate -------------------------------------------------------
if [ "$SKIP_BUILD" = "1" ]; then
  echo; echo "--skip-build given; stopping before the build."
  exit 0
fi

say "build"
command -v nvcc >/dev/null 2>&1 || fatal "nvcc not found — you booted a runtime image; you need nvidia/cuda:*-devel-*"
nvcc --version | tail -1
make gpu || { echo "BUILD FAILED" >&2; exit 2; }

say "known-answer gate"
echo "This is the port's correctness gate: the CUDA kernel must reproduce the"
echo "constants pinned before any CUDA existed (ADR 0001)."
hr
if ./tests/known_answer_gpu --device "$DEVICE"; then
  hr
  echo "GATE PASSED — the GPU computes the frozen challenge math."
  echo
  echo "Next: start the prover and point the verifier at it."
  echo
  echo "  ./prover_cuda 9099 --trace &"
  echo "  ./verifier --port 9099 --mb 4096 --k 262144 --lanes 16384 \\"
  echo "             --n 200 --warmup 20 --json results/gpu_trial.json"
  echo
  echo "Why those numbers. lanes is one GPU thread each, so the CPU default of 32"
  echo "leaves the card idle and measures launch overhead and nothing else; 16384"
  echo "is enough threads to have the memory system working. The defended default"
  echo "is --chain dependent, and the verifier refuses k/lanes < 16, so k has to"
  echo "rise with lanes: 262144/16384 is exactly the floor of 16."
  echo
  echo "Expect the useful-bytes bandwidth to look poor. An 8-byte probe drags a"
  echo "32-byte sector at minimum, so most of the traffic is thrown away. That is"
  echo "a real property of the current probe shape, not a bug, and measuring it is"
  echo "what justifies widening the probe in a later wire revision."
  exit 0
else
  hr
  echo "GATE FAILED — do not take any timing number from this build." >&2
  exit 2
fi
