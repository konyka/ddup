# ddup 开发路线图

开发方式：TDD。每个小步骤 = 先写失败测试 → 实现 → 全部测试通过 → commit + push。

- [x] **Phase 0 — 脚手架**
  CMake 工程、C 标准探测（C23→C99）、自研测试框架、PAL 骨架（时间/平台宏）、
  四平台 CI（Windows/Linux/macOS/FreeBSD）、文档骨架
- [x] **Phase 1 — RESP 协议**
  RESP2 解析器（含流式/分包边界）、写出器、解析-回写一致性随机测试、RESP3 类型
- [ ] **Phase 2 — 内存 KV 核心**
  arena 分配器、对象池、Robin Hood 哈希表 + 增量 rehash、命令分发表、
  PING/ECHO/GET/SET/DEL/EXISTS/INCR/DECR/APPEND/STRLEN/MGET/MSET
- [ ] **Phase 3 — 网络服务器**
  Windows IOCP、Linux epoll、macOS/FreeBSD kqueue 事件循环、TCP 监听、
  连接生命周期、pipelining、socket 级集成测试、压测客户端与基准记录
- [ ] **Phase 4 — 过期与淘汰**
  EXPIRE/TTL/PTTL/PERSIST、惰性 + 主动过期扫描、maxmemory 与 LRU 近似淘汰
- [ ] **Phase 5 — 复杂数据结构**
  Hash/List/Set/ZSet（跳表）、MULTI/EXEC/DISCARD/WATCH、SUBSCRIBE/PUBLISH
- [ ] **Phase 6 — 持久化与配置**
  AOF（追加 + 重写）、RDB 风格快照、配置文件、信号处理优雅退出
- [ ] **Phase 7+ — 长期**
  复制、集群模式、TLS、io_uring 优化落地、SIMD 解析优化、与 Garnet/Redis 基准对比
