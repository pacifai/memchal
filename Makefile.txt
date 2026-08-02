CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -pthread
# Add -march=native locally for best codegen (kept off by default for portability):
#   make CXXFLAGS="-O3 -std=c++17 -march=native -pthread"

all: prover verifier cheating_prover

prover: prover.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o $@ prover.cpp

verifier: verifier.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o $@ verifier.cpp

cheating_prover: cheating_prover.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o $@ cheating_prover.cpp

clean:
	rm -f prover verifier cheating_prover

.PHONY: all clean
