# ddup

redis c 的另一种实现 —— 参考微软 [Garnet](https://github.com/microsoft/garnet) 的设计，
用 C（C99 起步，自动探测编译器支持的最高标准，最高 C23）重新实现的 RESP 兼容缓存存储。

## 特性目标

- RESP2/RESP3 协议兼容，可被未修改的 Redis 客户端直接使用
- 性能优先：thread-per-core 无共享模型、平台最优 IO 多路复用
  （Linux: epoll/io_uring；macOS/FreeBSD: kqueue；Windows: IOCP）、
  零拷贝解析、arena/对象池内存管理
- 数据类型：String/Hash/List/Set/ZSet（跳表）；事务 MULTI/EXEC/WATCH；
  发布订阅（含 Redis 7 分片频道 SSUBSCRIBE/SPUBLISH）；过期与
  maxmemory 淘汰
- 脚本：Lua 5.1 嵌入（EVAL/EVALSHA/SCRIPT，vendored deps/lua，
  base/string/table/math 沙箱，redis.call/pcall，效果复制语义）
- 持久化：AOF（命令流追加 + 启动重放）与 RDB 风格二进制快照
  （原子写、定时自动保存）
- 复制：master/replica 全量同步 + PSYNC 部分重同步、只读副本、
  断线自动重连重同步、链式复制
- TLS：可选 OpenSSL 支持（独立 tls-port，与明文端口并行；集群总线可用
  `tls-cluster yes` 启用非阻塞 TLS；Linux/macOS CI
  安装 OpenSSL 并运行 TLS 测试；FreeBSD CI 不安装 OpenSSL）
- 多节点集群模式：Redis cluster 风格（16384 槽、gossip 节点发现、
  ADDSLOTS/SETSLOT、-MOVED/-ASK 重定向、MIGRATE 在线迁移、
  config epoch 冲突裁决、副本自动故障转移、ddup-reshard 运维工具、
  真实 Redis 总线协议互操作 cluster-bus-protocol redis）
- 多数据库与安全：16 逻辑库（SELECT/SWAPDB）、requirepass/AUTH、
  commandstats 统计
- 多线程：thread-per-core mt 模式（--io-threads N，槽路由 shared-nothing；
  集群和复制仅支持单线程，io_uring op 模式仅支持单线程 Linux）
- 跨平台：Windows / Linux / macOS / FreeBSD（其他 POSIX 系统走通用路径）
- TDD 开发：每个模块先写测试，全部测试通过后才提交

## 构建

依赖：CMake ≥ 3.20，任一现代 C 编译器（MSVC / Clang / GCC）。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

构建系统会自动探测编译器支持的最高 C 标准（C23 → C17 → C11 → C99）。

## 测试

```sh
ctest --test-dir build --output-on-failure
# 或
cmake --build build --target check
```

## 运行

```sh
# 默认端口 6379，无持久化
./build/ddup-server

# 指定配置文件 + 命令行覆盖（redis 风格 --key value）
./build/ddup-server ddup.conf --port 6380 --appendonly yes

# 压测（另开一个终端）
./build/ddup-bench -p 6379 -n 100000 -c 50 -P 16 -t set
./build/ddup-bench -p 6379 -n 100000 -c 50 -P 16 -t get
```

配置项见仓库根目录的 [ddup.conf](ddup.conf)（bind/port/maxmemory/
maxmemory-policy/dir/appendonly/appendfilename/dbfilename/save）。

持久化：`appendonly yes` 开启 AOF（启动时自动重放）；否则启动时加载
`dbfilename` 快照（`save N` 开启每 N 秒自动快照，SAVE 命令手动快照）。
SIGINT/SIGTERM 或 SHUTDOWN 命令优雅退出：AOF 必定落盘，配置了 save
间隔时额外写一次快照。

## TLS（可选，需 OpenSSL）

构建时找到 OpenSSL 即自动启用（`-DDDUP_TLS=OFF` 可强制关闭；未找到时
编译为 stub，`tls-port` 启动会报明确错误）。

```sh
# 生成自签名证书（示例）
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
    -days 3650 -nodes -subj "/CN=localhost"

./build/ddup-server --tls-port 6380 \
    --tls-cert-file cert.pem --tls-key-file key.pem

# 客户端连接（redis-cli）
redis-cli -p 6380 --tls --insecure

# 集群总线 TLS（要求 cluster-enabled yes；复用同一 cert/key）
./build/ddup-server --cluster-enabled yes --tls-cluster yes \
    --tls-cert-file cert.pem --tls-key-file key.pem
```

## 文档

- [架构设计](docs/architecture.md)
- [开发路线图](docs/roadmap.md)
- [性能基准](docs/performance.md)

## 许可证

见 [LICENSE](LICENSE)。
