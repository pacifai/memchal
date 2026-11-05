FROM debian:stable-slim

RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends \
      g++ make python3 python3-matplotlib python3-numpy procps ca-certificates \
      libargon2-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["make"]
