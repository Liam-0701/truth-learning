FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libcurl4-openssl-dev \
    libpoppler-cpp-dev \
    libsqlite3-dev \
    libzip-dev \
    ninja-build \
    pkg-config \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  && cmake --build build --parallel

FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    libcurl4 \
    libpoppler-cpp0v5 \
    libsqlite3-0 \
    libzip4 \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/truth_learning_server /app/truth_learning_server
COPY public /app/public

RUN mkdir -p /app/data /app/uploads

ENV PORT=8080 \
    TRUTH_DB_PATH=/app/data/truth-learning.db \
    TRUTH_UPLOAD_DIR=/app/uploads \
    TRUTH_PUBLIC_DIR=/app/public

VOLUME ["/app/data", "/app/uploads"]
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD curl --fail http://127.0.0.1:8080/api/health || exit 1

CMD ["/app/truth_learning_server"]

