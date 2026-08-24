FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        git \
        lld \
        llvm \
        llvm-dev \
        libgc-dev \
        liblld-dev \
        make \
        pkg-config \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
COPY . /work

RUN make -j1

CMD ["./build/vixc", "src/main.vix", "--check"]
