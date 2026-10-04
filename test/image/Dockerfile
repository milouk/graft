# What vmm/build-image.exp and a software-emulated boot need, in one image.
FROM debian:stable-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends qemu-system-x86 qemu-utils \
    expect netcat-openbsd openssh-client curl ca-certificates gawk \
 && rm -rf /var/lib/apt/lists/*
