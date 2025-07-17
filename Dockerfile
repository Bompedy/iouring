FROM ubuntu:22.04

# Install essentials
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    gdb \
    liburing-dev \
    ssh

# Allow io_uring syscalls
RUN echo "kernel.io_uring_disabled = 0" >> /etc/sysctl.conf

WORKDIR /workspace