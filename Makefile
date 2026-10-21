CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -pthread
# Add -march=native locally for best codegen (kept off by default for portability):
#   make CXXFLAGS="-O3 -std=c++17 -march=native -pthread"

# --- GPU build ---------------------------------------------------------------
# The CPU targets never need nvcc, so `make` works unchanged on a box without
# CUDA. `make gpu` builds the GPU prover and its known-answer gate.
#
# One PTX floor (compute_75) plus cubins for the cards worth renting, so a
# single binary runs on whatever the pod hands you: sm_80 A100, sm_86 for the
# cheap Ampere dev cards, sm_90 H100/H200. Anything newer JITs from the PTX.
NVCC      ?= nvcc
CUDA_HOME ?= /usr/local/cuda
NVCCFLAGS ?= -O3 -std=c++17 -Isrc \
             -gencode arch=compute_75,code=compute_75 \
             -gencode arch=compute_80,code=sm_80 \
             -gencode arch=compute_86,code=sm_86 \
             -gencode arch=compute_90,code=sm_90
CUDA_LIBS ?= -L$(CUDA_HOME)/lib64 -lcudart

all: prover verifier cheating_prover

prover: src/prover.cpp src/common.hpp src/challenge.hpp src/net.hpp src/segments.hpp src/weights.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/prover.cpp

verifier: src/verifier.cpp src/common.hpp src/challenge.hpp src/net.hpp src/segments.hpp src/weights.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/verifier.cpp

cheating_prover: src/cheating_prover.cpp src/common.hpp src/challenge.hpp src/net.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/cheating_prover.cpp

# --- GPU targets -------------------------------------------------------------
gpu: prover_cuda cheating_prover_cuda tests/known_answer_gpu libmemchal_weights.so

# Only this object needs nvcc. It includes challenge.hpp and the CUDA runtime
# and nothing else — no POSIX headers reach the device compiler.
gpu_answer.o: src/gpu_answer.cu src/gpu_answer.hpp src/challenge.hpp src/chacha20.hpp src/segments.hpp
	$(NVCC) $(NVCCFLAGS) -c src/gpu_answer.cu -o $@

# The same object, position-independent, for the shared library an inference
# engine loads (vllm_plugin/). A separate object so the binaries above keep
# the code they were measured with.
gpu_answer_pic.o: src/gpu_answer.cu src/gpu_answer.hpp src/challenge.hpp src/chacha20.hpp src/segments.hpp
	$(NVCC) $(NVCCFLAGS) -Xcompiler -fPIC -c src/gpu_answer.cu -o $@

# What the vLLM plugin loads with ctypes: the attached answer path plus a
# serving thread, behind plain C entry points (src/weights_server.cpp).
libmemchal_weights.so: src/weights_server.cpp src/gpu_resident.hpp src/gpu_answer.hpp src/net.hpp gpu_answer_pic.o
	$(CXX) $(CXXFLAGS) -fPIC -shared -Isrc -o $@ src/weights_server.cpp gpu_answer_pic.o $(CUDA_LIBS) \
	      -Wl,-rpath,$(CUDA_HOME)/lib64

# These two are ordinary C++: they include gpu_answer.hpp, which is free of CUDA
# types, so they compile with g++ and only need libcudart at link time.
prover_cuda: src/prover_cuda.cpp src/common.hpp src/challenge.hpp src/net.hpp src/gpu_answer.hpp src/gpu_resident.hpp src/weights.hpp src/segments.hpp gpu_answer.o
	$(CXX) $(CXXFLAGS) -Isrc -o $@ src/prover_cuda.cpp gpu_answer.o $(CUDA_LIBS)

cheating_prover_cuda: src/cheating_prover_cuda.cpp src/common.hpp src/challenge.hpp src/net.hpp src/gpu_answer.hpp gpu_answer.o
	$(CXX) $(CXXFLAGS) -Isrc -o $@ src/cheating_prover_cuda.cpp gpu_answer.o $(CUDA_LIBS)

tests/known_answer_gpu: tests/known_answer_gpu.cpp src/challenge.hpp src/gpu_answer.hpp gpu_answer.o
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/known_answer_gpu.cpp gpu_answer.o $(CUDA_LIBS)

# Report whether this box can build the GPU targets at all.
gpu-check:
	@command -v $(NVCC) >/dev/null 2>&1 \
	  && { echo "nvcc: $$($(NVCC) --version | tail -1)"; } \
	  || { echo "nvcc: NOT FOUND — GPU targets unavailable on this box"; exit 1; }
	@command -v nvidia-smi >/dev/null 2>&1 \
	  && nvidia-smi --query-gpu=name,memory.total,compute_cap --format=csv,noheader \
	  || echo "nvidia-smi: not found (no driver visible)"

clean:
	rm -f prover verifier cheating_prover tests/known_answer tests/segments_test \
	      prover_cuda cheating_prover_cuda tests/known_answer_gpu gpu_answer.o \
	      gpu_answer_pic.o libmemchal_weights.so

.PHONY: all gpu gpu-check clean
