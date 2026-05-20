# Stage 1: Build
FROM ubuntu:24.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libzmq3-dev \
    && rm -rf /var/lib/apt/lists/*

# Set up build directory
WORKDIR /build

# Copy the entire workspace into the builder
# We need sibling directories L3KV and lib for the build
COPY L3KV /L3KV
COPY l3kvg /l3kvg
COPY lib /lib
COPY libconveyor /libconveyor
COPY yyjson /yyjson

# Build L3KVG
WORKDIR /l3kvg
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) l3kvg_server

# Stage 2: Runtime
FROM ubuntu:24.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libzmq5 \
    libstdc++6 \
    ca-certificates \
    jq \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the built binary
COPY --from=builder /l3kvg/build/l3kvg_server .
COPY --from=builder /l3kvg/src/server/config.json . 

# Create data directory
RUN mkdir -p /app/data

# Default ports
EXPOSE 8080 8081

# The entrypoint will generate config.json from ENV variables
COPY l3kvg/docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["./l3kvg_server", "config.json"]
