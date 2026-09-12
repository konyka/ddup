#!/usr/bin/env python3
"""TDD checks for truthful benchmark comparison labels."""
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "benchmark_report", ROOT / "tools" / "generate_benchmark_report.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def main():
    assert MODULE.classify_server_identity(
        "redis-server", "Redis server v=7.2.15 sha=deadbeef") == "Redis 7.2.15"
    assert MODULE.classify_server_identity(
        "valkey-server", "Valkey server 9.0.4") == "Valkey 9.0.4"
    assert MODULE.classify_server_identity(
        "cache-server", "cache-server build 3.1") == "cache-server 3.1"
    assert MODULE.classify_server_identity("redis-server", "") == "Redis"
    print("benchmark report identity tests: ok")


if __name__ == "__main__":
    main()
