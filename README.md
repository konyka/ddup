# ddup

redis c 的另一种实现 —— 参考微软 [Garnet](https://github.com/microsoft/garnet) 的设计，
用 C（C99 起步，自动探测编译器支持的最高标准，最高 C23）重新实现的 RESP 兼容缓存存储。

## 特性目标

- RESP2/RESP3 协议兼容，可被未修改的 Redis 客户端直接使用
- 性能优先：thread-per-core 无共享模型、平台最优 IO 多路复用
  （Linux: io_uring/epoll，macOS/FreeBSD: kqueue，Windows: IOCP）、
  零拷贝解析、arena/对象池内存管理
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

## 文档

- [架构设计](docs/architecture.md)
- [开发路线图](docs/roadmap.md)
- [性能基准](docs/performance.md)

## 许可证

见 [LICENSE](LICENSE)。
