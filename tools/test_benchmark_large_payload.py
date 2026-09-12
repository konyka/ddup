#!/usr/bin/env python3
"""Regression test for ddup-bench receive buffers at large pipeline replies."""
import socket
import subprocess
import time
from pathlib import Path


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_port(port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("server did not listen")


def main():
    root = Path(__file__).resolve().parents[1]
    server = root / "build/ddup-server"
    bench = root / "build/ddup-bench"
    if not server.exists() or not bench.exists():
        raise SystemExit("ddup-server and ddup-bench must be built")
    invalid = subprocess.run(
        [str(bench), "-n", "1", "-c", "1", "-P", "9223372036854775807",
         "-d", "16", "-t", "ping"],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    if invalid.returncode == 0 or "dimensions too large" not in invalid.stderr:
        raise AssertionError("oversized benchmark dimensions were not rejected:\n" +
                             invalid.stdout + invalid.stderr)
    bounded = subprocess.run(
        [str(bench), "-n", "1", "-c", "1", "-P", "1048577", "-d", "16",
         "-t", "ping"],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    if bounded.returncode == 0 or "dimensions too large" not in bounded.stderr:
        raise AssertionError("unbounded benchmark dimensions were accepted:\n" +
                             bounded.stdout + bounded.stderr)
    invalid_port = subprocess.run(
        [str(bench), "-p", "-1", "-n", "1", "-c", "1", "-P", "1",
         "-t", "ping"],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    if invalid_port.returncode == 0 or "invalid port" not in invalid_port.stderr:
        raise AssertionError("invalid benchmark port was accepted:\n" +
                             invalid_port.stdout + invalid_port.stderr)
    malformed = subprocess.run(
        [str(bench), "-n", "12x", "-c", "1", "-P", "1", "-t", "ping"],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    if malformed.returncode == 0 or "invalid numeric argument" not in malformed.stderr:
        raise AssertionError("malformed benchmark number was accepted:\n" +
                             malformed.stdout + malformed.stderr)
    overflow = subprocess.run(
        [str(bench), "-n", "999999999999999999999999", "-c", "1", "-P", "1",
         "-t", "ping"],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    if overflow.returncode == 0 or "invalid numeric argument" not in overflow.stderr:
        raise AssertionError("overflowing benchmark number was accepted:\n" +
                             overflow.stdout + overflow.stderr)
    out_of_range = subprocess.run(
        [str(bench), "-p", "65536", "-n", "1", "-c", "1", "-P", "1",
         "-t", "ping"],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    if out_of_range.returncode == 0 or "invalid port" not in out_of_range.stderr:
        raise AssertionError("out-of-range benchmark port was accepted:\n" +
                             out_of_range.stdout + out_of_range.stderr)
    reserved = socket.socket()
    reserved.bind(("127.0.0.1", 0))
    unavailable = subprocess.run(
        [str(bench), "-p", str(reserved.getsockname()[1]), "-n", "1", "-c", "1",
         "-P", "1", "-t", "ping"], check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if unavailable.returncode == 0 or "connect failed" not in unavailable.stderr:
        reserved.close()
        raise AssertionError("connection failure path was not handled:\n" +
                             unavailable.stdout + unavailable.stderr)
    reserved.close()
    port = free_port()
    proc = subprocess.Popen(
        [str(server), "--port", str(port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        wait_port(port)
        common = [str(bench), "-p", str(port), "-n", "128", "-c", "1",
                  "-P", "64", "-d", "1024"]
        subprocess.run(common + ["-t", "set"], check=True,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True)
        result = subprocess.run(common + ["-t", "get"], check=False,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True)
        if result.returncode != 0:
            raise AssertionError("large-payload GET benchmark failed:\n" +
                                 result.stdout + result.stderr)
        if "128 requests completed" not in result.stdout:
            raise AssertionError("benchmark did not complete all replies:\n" +
                                 result.stdout + result.stderr)
        large = [str(bench), "-p", str(port), "-n", "1", "-c", "1",
                 "-P", "1", "-d", "131072"]
        subprocess.run(large + ["-t", "set"], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True)
        result = subprocess.run(large + ["-t", "get"], check=False,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True)
        if result.returncode != 0 or "1 requests completed" not in result.stdout:
            raise AssertionError("large single-reply GET benchmark failed:\n" +
                                 result.stdout + result.stderr)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)


if __name__ == "__main__":
    main()
