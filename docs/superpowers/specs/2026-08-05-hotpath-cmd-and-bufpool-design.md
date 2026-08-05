# 热路径命令 ID 表与缓冲池设计文档

## 背景与目标

ddup 当前命令分发采用 `if/else` 链逐次 `ci_equal()` 比较命令名，最坏情况要扫过整个链；同时 `resp_buf_reserve()`、接收缓冲增长、`conn_create()`/`conn_free()` 等热路径存在大量 `malloc`/`realloc`/`free`。参考 Garnet：

- `RespCommandHashLookup`：静态命令名哈希表，命令 → 稳定数字 ID，O(1) 路由。
- `LimitedFixedBufferPool`：分级固定大小缓冲池，减少网络层内存分配。

本次子项目为 ddup 引入这两块优化：**稳定命令 ID 表** + **分级缓冲池**，保持 RESP 兼容与现有 API 不变，全量测试必须继续通过。

## 范围

1. **命令 ID 表**
   - 在 `src/core/command.c` 中合并 `CMD_ARITY` / `WRITE_COMMANDS` 为统一的 `cmd_entry` 表。
   - 为每个命令分配稳定 16-bit ID（1..N），保留 0 表示 `CMD_UNKNOWN`。
   - 实现大小写不敏感命令名哈希表（开放寻址），从 `cmd_resolve(name, len)` 返回 ID。
   - 在 `command_dispatch()` 顶部解析命令名到 ID，后续用 `switch (cmd_id)` 分发。
   - `is_write_command()`、`queue_validate()`、`cluster_keyless()` 改为按 ID 查表/位掩码，O(1)。
   - 新增 `tests/test_cmdid.c` 用 TDD 覆盖：ID 唯一、名称查找大小写不敏感、未知命令、标志位正确。

2. **分级缓冲池**
   - 新增 `src/core/buf_pool.h|c`：提供固定尺寸分级（4 KiB / 16 KiB / 64 KiB / 256 KiB）的缓冲池。
   - API：`buf_pool_init/destroy`、`buf_pool_get(size)`、`buf_pool_put(ptr, size)`。
   - 热路径零 malloc：优先从池中取缓冲，释放时归还；超大尺寸回退 `malloc`/`free`。
   - 接入点：
     - `conn->rbuf`：创建时从池取 64 KiB，增长时换到更大一级或回退 realloc，释放时归还。
     - `conn->out`（`resp_buf`）：当缓冲区属于连接输出时，通过 `resp_buf_pool_reserve()` 从池扩容，连接释放时归还。
   - 新增 `tests/test_buf_pool.c` 用 TDD 覆盖：池命中/未命中、多次借还稳定、超大尺寸回退、destroy 后无泄漏（通过 valgrind/ASan 暗示，本地至少跑测试）。

3. **基准**
   - 在 `bench/bench_core.c` 或 `bench/ddup-bench.c` 增加可对比的 micro-benchmark：
     - 命令名解析：旧 `ci_equal` 链 vs 新哈希表。
     - 缓冲池：对比直接 malloc/free 与池化 borrow/put。
   - 记录数字到 `docs/performance.md`。

4. **文档**
   - 更新 `docs/architecture.md` 命令分发与内存管理小节。
   - 更新 `docs/roadmap.md` 新增 Phase 9 并勾选。

## 非目标

- 不改动 RESP 协议解析器、存储引擎数据结构、集群总线协议。
- 不替换 `malloc` 作为 DB value 的长期分配器（对象池是后续独立子项目）。
- 不引入第三方库；缓冲池用标准 `malloc/free` 做底层，池本身无锁（单 IO 线程）。

## 方案对比

### 命令 ID 表

| 方案 | 描述 | 优点 | 缺点 |
|---|---|---|---|
| A. 构建期生成完美哈希 | CMake/Python 脚本预计算 MPH，生成 C 数组 | 编译期固定，最小运行时开销 | 增加构建依赖与生成脚本，调试复杂 |
| B. 运行时构建开放寻址表（推荐） | 编译期静态 `cmd_entry[]`，启动时一次建成 hash table | 实现简单、可测试、仍 O(1) | 启动时一次线性插入 |
| C. 给现有线性表加 ID | 保留 `if/else` 链，仅给命令编号 | 改动最小 | 未解决分发性能问题 |

**推荐 B**：在零第三方依赖、可维护性与性能间最佳平衡。启动一次构建表对 server 可忽略。

### 缓冲池

| 方案 | 描述 | 优点 | 缺点 |
|---|---|---|---|
| A. 分级固定缓冲池（推荐） | 4/16/64/256 KiB 四级自由列表 | 命中时零分配、缓存友好、实现简单 | 需要按尺寸归还，超大回退 malloc |
| B. 只升级 arena | 让 arena block 更大或复用 | 与现有 arena 集成 | 不解决 rbuf/out 的独立大块分配 |
| C. 替换全局 malloc | hook malloc/free | 对所有分配生效 | 跨平台复杂、超出本项目范围 |

**推荐 A**：直接针对已识别的两个热路径分配点。

## 架构

```
┌─────────────────────────────────────────────┐
│  命令层                                        │
│  command_dispatch() → cmd_resolve(name, len)  │
│                     → switch (cmd_id)         │
│  is_write_command(cmd_id)  O(1)               │
│  queue_validate(cmd_id)    O(1)               │
├─────────────────────────────────────────────┤
│  cmd_table (src/core/command.c)               │
│  cmd_entry { name, id, min/max, parity, flags }│
│  cmd_hash 开放寻址表 (大小固定，运行时初始化)    │
├─────────────────────────────────────────────┤
│  缓冲池 (src/core/buf_pool.c)                  │
│  4/16/64/256 KiB 自由列表，单线程              │
├─────────────────────────────────────────────┤
│  server.c conn_create/free / read loop        │
│  resp_buf_reserve() 池化路径                  │
└─────────────────────────────────────────────┘
```

## 关键接口

### 命令 ID 表

```c
/* command.h */
#define CMD_ID_UNKNOWN 0

typedef enum {
    CMD_PING = 1,
    CMD_ECHO,
    CMD_GET,
    CMD_SET,
    /* ... stable IDs ... */
} cmd_id;

/* Resolve command name to stable ID; case-insensitive. */
uint16_t cmd_resolve(const char *name, size_t len);

/* O(1) helpers after resolution. */
int cmd_is_write(uint16_t cmd_id);
int cmd_min_argc(uint16_t cmd_id);
int cmd_max_argc(uint16_t cmd_id);
int cmd_parity(uint16_t cmd_id);
```

### 缓冲池

```c
/* buf_pool.h */
typedef struct buf_pool buf_pool;

int buf_pool_init(buf_pool *pool);
void buf_pool_destroy(buf_pool *pool);

/* Borrow a buffer of at least size bytes. actual_size receives the real size. */
void *buf_pool_get(buf_pool *pool, size_t size, size_t *actual_size);

/* Return a buffer previously obtained with the same size. */
void buf_pool_put(buf_pool *pool, void *ptr, size_t size);
```

### resp_buf 池化扩展

```c
/* Reserve out buffer, preferring pooled buffers when owner is a connection. */
void resp_buf_pool_reserve(resp_buf *b, size_t n, buf_pool *pool);
```

## 测试策略

- `tests/test_cmdid.c`：
  - 所有命令 ID 唯一且非零。
  - 大小写不敏感解析（`GET`/`get`/`Get` → `CMD_GET`）。
  - 未知命令返回 `CMD_ID_UNKNOWN`。
  - `is_write_command` 与 `WRITE_COMMANDS` 旧列表一致。
  - `queue_validate` 对合法/非法 arity 返回正确。
- `tests/test_buf_pool.c`：
  - 借 64 KiB 得到不小于 64 KiB 且对齐的缓冲。
  - 同一缓冲多次借还地址稳定。
  - 超大尺寸（> 最大 tier）成功并正确释放。
  - `destroy` 后无崩溃。
- 全量 `ctest`：默认 C23 + 强制 C99 双路径 38/38 通过，无新增警告。

## 性能考量

- 命令解析：从 O(N) 字符串比较 → O(1) 哈希 + 一次 `switch`。
- 缓冲池：命中时完全避免 `malloc`/`free`，减少 TLB/缓存压力。
- 单 IO 线程：缓冲池无需原子/锁，借用/归还为指针移动。
- 退化路径：未知命令仍走慢路径；超大缓冲回退 malloc，不影响正确性。

## 风险与回退

- 命令 ID 改变会破坏未来 AOF/复制/集群序列化；本次 IDs 一旦确定即视为稳定。
- 缓冲池归还尺寸错误会导致内存错乱；`buf_pool_put` 断言 size 属于某一级，越界则 `free`。
- 若池容量不足（突发连接），回退 malloc 保证可用。
- 所有改动后若 `bench_core` 或 `ddup-bench` 下降，保留原路径为编译选项。

## 文档更新

- `docs/architecture.md`：补充“命令 ID 与 O(1) 分发”、“分级缓冲池”两节。
- `docs/performance.md`：新增 micro-benchmark 数字。
- `docs/roadmap.md`：新增 Phase 9 并勾选。
