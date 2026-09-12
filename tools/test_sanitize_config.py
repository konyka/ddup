#!/usr/bin/env python3
"""TDD regression for the thread-sanitizer CMake configuration."""
import shutil
import subprocess
import tempfile
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    build = Path(tempfile.mkdtemp(prefix="ddup-tsan-config-"))
    try:
        proc = subprocess.run(
            ["cmake", "-S", str(root), "-B", str(build),
             "-DCMAKE_BUILD_TYPE=Debug", "-DDDUP_SANITIZE=thread"],
            text=True, capture_output=True, check=False,
        )
        if proc.returncode:
            raise AssertionError("thread sanitizer configuration failed:\n" +
                                 proc.stdout + proc.stderr)
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
        if "DDUP_SANITIZE:STRING=thread" not in cache:
            raise AssertionError("thread sanitizer was not recorded in CMake cache")
        print("thread sanitizer configuration: ok")
    finally:
        shutil.rmtree(build, ignore_errors=True)


if __name__ == "__main__":
    main()
