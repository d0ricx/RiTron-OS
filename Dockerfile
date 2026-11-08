FROM rust:1.85.1-slim-bookworm

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        ca-certificates \
        g++-mingw-w64-x86-64 \
        make \
        python3 \
        xorriso \
    && rm -rf /var/lib/apt/lists/*

RUN rustup target add x86_64-pc-windows-gnu

WORKDIR /workspace

CMD ["make", "iso", "disk"]
