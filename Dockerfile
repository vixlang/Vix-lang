FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gpg \
    && sed -i 's|http://archive.ubuntu.com|https://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i 's|http://security.ubuntu.com|https://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        git \
        make \
        pkg-config \
        python3 \
        zlib1g-dev \
        libzstd-dev \
    && curl -fsSL https://mirrors.tuna.tsinghua.edu.cn/llvm-apt/x86_64-snapshot/llvm-snapshot.asc | gpg --dearmor -o /usr/share/keyrings/llvm-snapshot.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] https://mirrors.tuna.tsinghua.edu.cn/llvm-apt/noble/ llvm-toolchain-noble-21 main" > /etc/apt/sources.list.d/llvm-21.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-21 \
        lld-21 \
        llvm-21-dev \
        liblld-21-dev \
        libgc-dev \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-21 100 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-21 100 \
    && ln -sf /usr/bin/llvm-config-21 /usr/bin/llvm-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
COPY . /work

RUN make -j1

CMD ["./build/vixc", "src/main.vix", "--check"]
