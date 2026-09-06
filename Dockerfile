# 多阶段构建：builder 编译，runtime 仅含运行依赖（非 root）
# 用法: docker build -t imserver .

# ---- 构建阶段 ----
FROM ubuntu:24.04 AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    cmake \
    make \
    libmysqlclient-dev \
    libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY client ./client
COPY tests ./tests
COPY third_party ./third_party
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" --target imserver

# ---- 运行阶段 ----
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    libmysqlclient24 \
    libhiredis1 \
    curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -m -u 1000 im
WORKDIR /app
COPY --from=builder /src/build/imserver ./imserver
USER im
EXPOSE 9191
# 连接配置经环境变量注入（IM_MYSQL_HOST 等）；连接失败将启动报错退出
ENTRYPOINT ["./imserver"]