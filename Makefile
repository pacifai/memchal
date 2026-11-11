CXX       ?= g++
CXXFLAGS  ?= -O3 -std=c++17 -Wall -Wextra -pthread
NVCC      ?= nvcc
CUDA_HOME ?= /usr/local/cuda
NVCCFLAGS ?= -O3 -std=c++17 \
             -gencode arch=compute_75,code=compute_75 \
             -gencode arch=compute_80,code=sm_80 \
             -gencode arch=compute_86,code=sm_86 \
             -gencode arch=compute_90,code=sm_90
CUDA_LIBS ?= -L$(CUDA_HOME)/lib64 -lcudart

BUILD := build
DEPFLAGS = -MMD -MP

CORE_BINS    := $(BUILD)/verifier $(BUILD)/cpu/prover
ATTACK_BINS  := $(BUILD)/attacks/cpu/cheating_prover
BENCH_BINS   := $(BUILD)/bench/build_cost
TEST_BINS    := $(BUILD)/tests/shared/challenge_kat $(BUILD)/tests/shared/argon2d_kat \
                $(BUILD)/tests/shared/chain_test \
                $(BUILD)/tests/shared/argon2d_differential $(BUILD)/tests/attacks/argon2d_rebuild_test \
                $(BUILD)/tests/attacks/seed_recovery_test
GPU_CORE_BINS   := $(BUILD)/gpu/prover $(BUILD)/tests/gpu/answer_kat
GPU_ATTACK_BINS := $(BUILD)/attacks/gpu/cheating_prover

all: core attacks $(BENCH_BINS)
core: $(CORE_BINS)
attacks: $(ATTACK_BINS)
tests: $(TEST_BINS)
gpu: $(GPU_CORE_BINS) $(GPU_ATTACK_BINS)
gpu-core: $(GPU_CORE_BINS)

$(BUILD)/verifier: src/verifier/main.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $<

$(BUILD)/cpu/%: src/cpu/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $<

$(BUILD)/bench/%: src/bench/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $<

$(BUILD)/tests/shared/argon2d_differential: tests/shared/argon2d_differential.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $< -largon2

$(BUILD)/tests/shared/%: tests/shared/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $<

$(BUILD)/attacks/cpu/%: src/attacks/cpu/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $<

$(BUILD)/tests/attacks/%: tests/attacks/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $<

$(BUILD)/gpu/%.o: src/gpu/%.cu
	@mkdir -p $(@D)
	$(NVCC) $(NVCCFLAGS) $(DEPFLAGS) -Isrc -c $< -o $@

$(BUILD)/gpu/prover: src/gpu/prover.cpp $(BUILD)/gpu/answer.o
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $^ $(CUDA_LIBS)

$(BUILD)/attacks/gpu/%.o: src/attacks/gpu/%.cu
	@mkdir -p $(@D)
	$(NVCC) $(NVCCFLAGS) $(DEPFLAGS) -Isrc -c $< -o $@

$(BUILD)/attacks/gpu/cheating_prover: src/attacks/gpu/cheating_prover.cpp $(BUILD)/gpu/answer.o \
                                      $(BUILD)/attacks/gpu/adversary.o
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $^ $(CUDA_LIBS)

$(BUILD)/tests/gpu/answer_kat: tests/gpu/answer_kat.cpp $(BUILD)/gpu/answer.o
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Isrc -o $@ $^ $(CUDA_LIBS)

gpu-check:
	@command -v $(NVCC) >/dev/null 2>&1 \
	  && { echo "nvcc: $$($(NVCC) --version | tail -1)"; } \
	  || { echo "nvcc: not found; GPU targets unavailable"; exit 1; }
	@command -v nvidia-smi >/dev/null 2>&1 \
	  && nvidia-smi --query-gpu=name,memory.total,compute_cap --format=csv,noheader \
	  || echo "nvidia-smi: not found"

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

.PHONY: all core attacks tests gpu gpu-core gpu-check clean
