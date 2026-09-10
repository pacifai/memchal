CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -pthread
# Add -march=native locally for best codegen (kept off by default for portability):
#   make CXXFLAGS="-O3 -std=c++17 -march=native -pthread"

all: prover verifier cheating_prover bench_build

prover: src/prover.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/prover.cpp

verifier: src/verifier.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/verifier.cpp

cheating_prover: src/cheating_prover.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/cheating_prover.cpp

# Build-cost benchmark for the memory-hard buffer generator (ADR 0007).
# Not a protocol participant: it measures what one 1 KiB block costs to build,
# which is the attacker's unit of work in the build-buffer design.
bench_build: src/bench_build.cpp src/argon2d.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/bench_build.cpp

clean:
	rm -f prover verifier cheating_prover bench_build \
	      tests/known_answer tests/known_answer_argon2d

.PHONY: all clean
