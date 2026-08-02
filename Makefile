CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -pthread
# Add -march=native locally for best codegen (kept off by default for portability):
#   make CXXFLAGS="-O3 -std=c++17 -march=native -pthread"

all: prover verifier cheating_prover

prover: src/prover.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/prover.cpp

verifier: src/verifier.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/verifier.cpp

cheating_prover: src/cheating_prover.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/cheating_prover.cpp

clean:
	rm -f prover verifier cheating_prover tests/kat

.PHONY: all clean
