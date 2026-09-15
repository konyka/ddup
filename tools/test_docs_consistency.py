#!/usr/bin/env python3
"""Guard implementation/documentation claims for recently completed phases."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main():
    architecture = (ROOT / "docs" / "architecture.md").read_text(encoding="utf-8")
    assert "`MEMORY MALLOC-STATS` 在固定栈缓冲区中生成 bulk allocator 统计文本" in architecture
    assert "`PURGE/MALLOC-STATS` 为无分配占位兼容响应" not in architecture
    assert "## Phase 521：MEMORY MALLOC-STATS 响应收敛" in architecture
    print("documentation consistency tests: ok")


if __name__ == "__main__":
    main()
