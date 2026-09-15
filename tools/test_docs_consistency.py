#!/usr/bin/env python3
"""Guard implementation/documentation claims for recently completed phases."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main():
    architecture = (ROOT / "docs" / "architecture.md").read_text(encoding="utf-8")
    redis_compat = (ROOT / "docs" / "redis-compat-audit.md").read_text(encoding="utf-8")
    command = (ROOT / "src" / "core" / "command.c").read_text(encoding="utf-8")
    assert "`MEMORY MALLOC-STATS` 在固定栈缓冲区中生成 bulk allocator 统计文本" in architecture
    assert "`PURGE/MALLOC-STATS` 为无分配占位兼容响应" not in architecture
    assert "## Phase 521：MEMORY MALLOC-STATS 响应收敛" in architecture
    assert '#define DDUP_REDIS_COMPAT_VERSION_LITERAL "8.10.1"' in command
    assert '"Redis ver. " DDUP_REDIS_COMPAT_VERSION_LITERAL "\\n"' in command
    assert "分布式锁脚本" in redis_compat
    assert "Garnet/单机缓存存储不适配项（如分布式锁脚本、阻塞语义）" not in redis_compat
    print("documentation consistency tests: ok")


if __name__ == "__main__":
    main()
