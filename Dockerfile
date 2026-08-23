# Toolchain image for the memory-challenge PoC.
#
# This image provides ONLY the build/run toolchain; the source tree is bind-
# mounted at /work at run time (see run.sh) so the container always builds the
# unmodified working copy and results land on the host.
#
# IMPORTANT: build/run as linux/arm64 (native on Apple Silicon). Do NOT use
# amd64 emulation — it destroys timing fidelity, which is the whole measurement.
FROM debian:stable-slim

# g++/make to build; python3 + matplotlib (via apt, no pip) for aggregation and
# plotting. procps gives /proc tooling; ca-certificates for completeness.
RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends \
      g++ make python3 python3-matplotlib python3-numpy procps ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["make"]
