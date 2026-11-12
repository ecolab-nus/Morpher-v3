FROM ubuntu:24.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
      build-essential \
      cmake \
      git \
      python3 \
      python3-venv \
      clang-18 \
      llvm-18 \
      llvm-18-dev \
      llvm-18-tools \
      lld-18 \
      libpolly-18-dev \
      gcc-multilib \
      g++-multilib \
      graphviz \
      procps && \
    rm -rf /var/lib/apt/lists/*

ENV VIRTUAL_ENV=/opt/venv
RUN python3 -m venv "${VIRTUAL_ENV}"

ENV PATH="/opt/venv/bin:/usr/lib/llvm-18/bin:${PATH}" \
    MORPHER_LLVM_BIN_DIR=/usr/lib/llvm-18/bin \
    MORPHER_LLVM_LIB_DIR=/usr/lib/llvm-18/lib \
    MORPHER_LLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm \
    PYTHONUNBUFFERED=1

WORKDIR /opt/morpher

# Cache Python dependencies separately from source changes.
COPY python_requirements.txt ./
RUN python -m pip install --no-cache-dir -r python_requirements.txt

COPY . .

# Limit parallel compilation to control memory use.
ARG BUILD_JOBS=2

RUN set -eu; \
    for component in \
      Morpher_DFG_Generator \
      Morpher_CGRA_Mapper \
      simulator/artifact_simulator; do \
        cmake -S "${component}" -B "${component}/build" \
          -DCMAKE_BUILD_TYPE=Release \
          -DLLVM_DIR="${MORPHER_LLVM_DIR}"; \
        cmake --build "${component}/build" \
          --parallel "${BUILD_JOBS}"; \
    done

RUN mkdir -p generated workspace

CMD ["/bin/bash"]