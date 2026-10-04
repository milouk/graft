# Linker and emulator for the bare-metal test kernel. Nothing else.
FROM debian:stable-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends lld qemu-system-x86 \
 && rm -rf /var/lib/apt/lists/*
