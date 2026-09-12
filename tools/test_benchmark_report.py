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
    original_run = MODULE.subprocess.run

    def timeout_run(*args, **kwargs):
        raise MODULE.subprocess.TimeoutExpired(kwargs.get("args", args[0]), 1)

    MODULE.subprocess.run = timeout_run
    try:
        try:
            MODULE.run_bench(["ddup-bench", "-t", "ping"])
        except RuntimeError as exc:
            assert "timed out" in str(exc)
        else:
            raise AssertionError("benchmark timeout must fail explicitly")
    finally:
        MODULE.subprocess.run = original_run

    def version_timeout(*args, **kwargs):
        raise MODULE.subprocess.TimeoutExpired(args[0], 1)

    MODULE.subprocess.run = version_timeout
    try:
        assert MODULE.server_identity("redis-server") == "Redis"
    finally:
        MODULE.subprocess.run = original_run

    payload = {
        "generated_at": "2026-09-12T00:00:00+00:00",
        "environment": "test",
        "cpu_count": 1,
        "baseline": {"requests": 1, "clients": 1, "pipelines": [1],
                      "value_size": 1},
        "results": [{"product": "</script><img src=x onerror=alert(1)>",
                     "mode": "ping", "clients": 1, "pipeline": 1,
                     "value_size": 1, "status": "ok", "rps": 1,
                     "p50": 1, "p99": 1, "min": 1, "max": 1}],
    }
    report = MODULE.render(payload)
    assert "</script><img" not in report
    assert "<\\/script><img" in report
    print("benchmark report identity tests: ok")


if __name__ == "__main__":
    main()
