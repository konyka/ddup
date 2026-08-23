# Redis 8.10.1 命令兼容性盘点

> 基线：Redis 8.10.1 官方 `src/commands/*.json`（459 个 JSON、459 条命令条目）。
> 对照：ddup `src/core/command.c` `CMD_TABLE`。
> 生成：`python3 tools/audit_redis_compat.py --fetch /tmp/redis810 --tag 8.10.1 --repo . --json`。
> 本文件维护 Redis 8 增量缺口；Redis 7.2.15 基线见 `docs/redis-compat-audit.md`。

## 总览

| 类别 | 数量 |
| --- | --- |
| 缺失顶层命令 | 21 |
| 整体缺失容器 | 3 |
| 已实现容器内缺失子命令 | 3 |

缺失集中在三块：

1. Redis 8.8 新增 `ARRAY` 类型（18 个 `AR*` 命令）。
2. Redis 7.4/8.0 hash 字段级 TTL 与 `HGETDEL/HGETEX/HSETEX`（13 个命令）。
3. Redis 8.2+ 的运维/诊断命令，以及少量 Cluster 子命令。

## 已实现增量

- Hash 字段级 TTL 全族：`HGETDEL/HGETEX/HSETEX` 与
  `HEXPIRE/HPEXPIRE/HEXPIREAT/HPEXPIREAT/HPERSIST/HTTL/HPTTL/
  HEXPIRETIME/HPEXPIRETIME`。`obj_hash` 增加独立 `expires` 表，字段
  TTL 为绝对毫秒时间戳；读路径惰性删除过期字段，全量视图先 purge，
  空 hash 自动删除 key。
- Set 基数：`SUNIONCARD/SDIFFCARD` 支持 `LIMIT` 提前停与 union 的
  `APPROX` 参数解析；复用 `obj_set` 双编码遍历，临时 `rh_table` 去重，
  不物化完整结果集。
- String safety/atomicity tranche: `INCREX` uses checked integer/long-double
  arithmetic and bounded expiry parsing; `MSETEX` validates the complete batch
  before mutation and supports `NX/XX/KEEPTTL/EX/PX/EXAT/PXAT`; `DELEX`
  supports conditional string deletion; `DIGEST` uses vendored XXH3 with
  fixed-width output. All four commands are covered by TDD tests.
- Cluster slot maintenance: `SFLUSH` intersects requested ranges with local
  ownership and returns coalesced flushed ranges; `TRIMSLOTS RANGES` validates
  ownership before deleting keys from unserved slots. Both paths collect keys
  before mutation, preserve expiry/accounting invariants, and fail closed on
  allocation errors.
- `CLUSTER SLOT-STATS` supports the low-overhead `key-count` metric for
  `SLOTSRANGE` and `ORDERBY [LIMIT] [ASC|DESC]`; memory/CPU/network metrics
  remain explicitly rejected until their accounting model is available.

## 实现策略（性能优先）

- 数据面命令优先实现，保证核心语义与复杂度级别一致。
- hash 字段 TTL 采用字段级绝对过期时间元数据，listpack 与 rh_table 两
  编码下均 O(fields) 查询/清理；过期字段惰性删除，读路径零额外 malloc。
- Set 基数复用 `setop_eval`/`obj_set` 遍历，`LIMIT` 提前停，不物化结果。
- List 多元素移动复用现有 `obj_list` 批量 pop/push 助手，避免逐元素复制。
- Stream 精确删除/PEL 控制复用现有 stream PEL 索引；`XIDMPRECORD` 作为
  内部命令做参数校验与幂等应答。
- Array 类型采用 `obj_array`：以 `rh_table` 存稀疏 index→value，O(1)
  随机读写，遍历按索引跨度扫描；`ARCARD/ARCOUNT` 与 `ARNEXT` 元数据常驻
  对象头，O(1)。
- 管理/危险命令优先做安全前置校验；不支持或无法等价实现的模块命令
  记录在下方范围外清单。

## 范围外 / 待后续评估

- `BACKUP`、`HIMPORT`、`HOTKEYS`：Redis 8 管理容器，涉及磁盘/RDB/热点
  采样；计划作为管理容器最小兼容实现或明确记录差异。
- `ARGREP RE`：需完整正则引擎；当前优先实现 `EXACT/MATCH/GLOB`，
  `RE` 返回明确错误或降级，后续引入 PAL 正则抽象。

## 审计基线（机器断言，勿手改格式）

<!-- AUDIT-BASELINE-START
missing_top: arcount ardel ardelrange arget argetrange argrep arinfo arinsert arlastitems arlen armget armset arnext arop arring arscan arseek arset backup himport hotkeys
missing_containers: backup himport hotkeys
missing_sub: cluster migration
missing_sub: cluster syncslots
AUDIT-BASELINE-END -->
