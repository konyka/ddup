#!/usr/bin/env python3
"""Run a reproducible ddup/reference-server matrix and emit JSON plus HTML."""
import argparse
import html
import json
import platform
import re
import shutil
import signal
import socket
import statistics
import subprocess
import time
from datetime import datetime
from pathlib import Path

RESULT_RE = re.compile(
    r"(?P<requests>\d+) requests completed in (?P<seconds>[0-9.]+) seconds.*?"
    r"latency \(us\): min=(?P<min>\d+) p50=(?P<p50>\d+) "
    r"p99=(?P<p99>\d+) max=(?P<max>\d+).*?"
    r"(?P<rps>[0-9.]+) requests per second", re.S)
BENCH_TIMEOUT_SECONDS = 120


def classify_server_identity(binary_name, version_output):
    """Return a truthful product label from a server's own version banner."""
    text = (version_output or "").lower()
    if "valkey" in text or "valkey" in binary_name.lower():
        product = "Valkey"
    elif "redis" in text or "redis" in binary_name.lower():
        product = "Redis"
    else:
        product = Path(binary_name).name
    match = re.search(r"(?:v(?:ersion)?[ =]?|\b)([0-9]+(?:\.[0-9]+){1,2})",
                      version_output or "", re.I)
    return "%s %s" % (product, match.group(1)) if match else product


def server_identity(path):
    """Probe a comparison server once, keeping failures explicit in reports."""
    try:
        proc = subprocess.run([str(path), "--version"], text=True,
                              capture_output=True, check=False)
        banner = proc.stdout + proc.stderr
    except OSError as exc:
        banner = str(exc)
    return classify_server_identity(Path(path).name, banner)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_port(port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.2):
                return
        except OSError:
            time.sleep(.02)
    raise RuntimeError("server did not listen")


def run_bench(command):
    try:
        proc = subprocess.run(command, text=True, capture_output=True,
                              check=False, timeout=BENCH_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("benchmark timed out after %ss: %s" %
                           (BENCH_TIMEOUT_SECONDS, " ".join(command))) from exc
    output = proc.stdout + proc.stderr
    if proc.returncode:
        raise RuntimeError("command failed: " + " ".join(command) + "\n" + output)
    match = RESULT_RE.search(output)
    if not match:
        raise RuntimeError("cannot parse benchmark output:\n" + output)
    values = match.groupdict()
    return {key: (float(values[key]) if key in ("seconds", "rps")
                  else int(values[key])) for key in
            ("requests", "seconds", "min", "p50", "p99", "max", "rps")}


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(3)


def case(product, bench, requests, clients, pipeline, mode, value_size):
    port = free_port()
    if product["kind"] == "ddup":
        command = [str(product["server"]), "--port", str(port)]
        if product["workers"] > 1:
            command += ["--io-threads", str(product["workers"])]
    else:
        command = [str(product["server"]), "--port", str(port), "--save", "",
                   "--appendonly", "no", "--protected-mode", "no"]
    proc = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, text=True)
    try:
        wait_port(port)
        common = [str(bench), "-p", str(port), "-n", str(requests), "-c",
                  str(clients), "-P", str(pipeline), "-d", str(value_size)]
        if mode == "get":
            run_bench(common + ["-t", "set"])
        result = run_bench(common + ["-t", mode])
    finally:
        stop(proc)
    result.update({"product": product["name"], "mode": mode, "clients": clients,
                   "pipeline": pipeline, "value_size": value_size})
    return result


def render(payload):
    rows = payload["results"]
    products = sorted({row["product"] for row in rows})
    cards = []
    for product in products:
        values = [row["rps"] for row in rows
                  if row["product"] == product and row.get("status") == "ok"]
        if not values:
            values = [0]
        cards.append("<div class='card'><b>%s</b><strong>%s</strong>"
                     "<small>median req/s</small></div>" %
                     (html.escape(product),
                      "{:,.0f}".format(statistics.median(values))))
    columns = ("product", "mode", "clients", "pipeline", "value_size", "status",
               "rps", "p50", "p99", "min", "max")
    table = "".join("<tr>%s</tr>" % "".join(
        "<td>%s</td>" % html.escape(str(row.get(key, "-"))) for key in columns)
                    for row in rows)
    data = json.dumps(rows, separators=(",", ":")).replace("</", "<\\/")
    failures = "".join("<li><b>%s %s P%s</b><pre>%s</pre></li>" %
                       (html.escape(str(row["product"])),
                        html.escape(str(row["mode"])),
                        html.escape(str(row["pipeline"])),
                        html.escape(str(row.get("error", "unknown error"))))
                       for row in rows if row.get("status") == "failed")
    if not failures:
        failures = "<li>无失败样本。</li>"
    base = payload["baseline"]
    return """<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'><title>ddup Benchmark Report</title>
<style>:root{--ink:#17212b;--muted:#61707b;--paper:#f5f1e8;--line:#d8d1c4;--accent:#d9573f}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:15px/1.5 Georgia,serif}main{max-width:1280px;margin:auto;padding:42px 24px 64px}h1{font-size:42px;line-height:1.05;margin:8px 0}.kicker{color:var(--accent);font:700 12px ui-monospace,monospace;letter-spacing:.15em;text-transform:uppercase}.lede{color:var(--muted);font-size:18px;max-width:800px}.cards{display:flex;gap:14px;flex-wrap:wrap;margin:24px 0}.card{background:#fffdf8;border:1px solid var(--line);padding:15px 18px;min-width:210px;display:grid;gap:3px}.card strong{font:700 28px ui-monospace,monospace}.card small{color:var(--muted)}.note{border-left:4px solid var(--accent);padding:10px 14px;background:#fff8ed}.scroll{overflow:auto;border:1px solid var(--line);background:#fffdf8}table{border-collapse:collapse;width:100%%;font:12px ui-monospace,monospace}th,td{padding:8px 10px;border-bottom:1px solid var(--line);text-align:right;white-space:nowrap}th{background:#eee8dc}th:first-child,td:first-child,th:nth-child(2),td:nth-child(2){text-align:left}canvas{width:100%%;height:360px;border:1px solid var(--line);background:#fffdf8}pre{white-space:pre-wrap;background:#1d2830;color:#e6eee9;padding:14px;font:12px ui-monospace,monospace}</style></head><body><main>
<div class='kicker'>Performance Lab / %s</div><h1>ddup Benchmark Report</h1>
<p class='lede'>同一 RESP 压测客户端、同一 loopback 环境和固定请求矩阵下，对 %s 进行吞吐和延迟对比。</p>
<div class='cards'>%s</div><div class='note'>Garnet 未安装，未纳入实测；报告不使用推测值。结果是单机开发环境测量，不代表生产部署结论。</div>
<h2>测试方法</h2><ul><li>请求数：%d；并发连接：%d；pipeline：%s；value：%d bytes。</li><li>操作：SET、GET、PING；GET 测量前先写入同规模 keyspace。</li><li>吞吐为完成请求数 / wall time；延迟为客户端观测的 log2 微秒桶。</li></ul>
<h2>吞吐对比</h2><canvas id='chart'></canvas><h2>原始结果</h2><div class='scroll'><table><thead><tr><th>product</th><th>mode</th><th>clients</th><th>pipeline</th><th>value</th><th>status</th><th>req/s</th><th>p50 us</th><th>p99 us</th><th>min us</th><th>max us</th></tr></thead><tbody>%s</tbody></table></div>
<h2>失败样本</h2><ul>%s</ul>
<h2>可复现命令</h2><pre>cmake -S . -B build -DCMAKE_BUILD_TYPE=Release\ncmake --build build -j2\npython3 tools/generate_benchmark_report.py --output reports/benchmark-report.html --json reports/benchmark-report.json</pre>
<footer>生成时间：%s<br>主机：%s；CPU：%s<br>原始 JSON：同目录下的 benchmark-*.json</footer>
<script>const data=%s;const c=document.getElementById('chart'),x=c.getContext('2d');function draw(){const d=devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;c.width=w*d;c.height=h*d;x.scale(d,d);x.clearRect(0,0,w,h);const g={};data.forEach(v=>{const k=v.mode+' / P'+v.pipeline;(g[k]??={})[v.product]=v.rps||0});const ks=Object.keys(g),ps=[...new Set(data.map(v=>v.product))],m=Math.max(1,...data.map(v=>v.rps||0));ks.forEach((k,i)=>ps.forEach((p,j)=>{const v=g[k][p]||0,b=Math.max(10,(w-80)/(ks.length*ps.length)),xx=55+(i*ps.length+j)*b,yy=h-35-v/m*(h-70);x.fillStyle=['#d9573f','#26736b','#405a80'][j%%3];x.fillRect(xx,yy,b-3,h-35-yy)}));x.strokeStyle='#a9a294';x.beginPath();x.moveTo(45,10);x.lineTo(45,h-35);x.lineTo(w-10,h-35);x.stroke()}addEventListener('resize',draw);draw();</script></main></body></html>""" % (html.escape(payload["generated_at"]), html.escape(", ".join(products)), "".join(cards), base["requests"], base["clients"], ", ".join(map(str, base["pipelines"])), base["value_size"], table, failures, html.escape(payload["generated_at"]), html.escape(payload["environment"]), html.escape(str(payload["cpu_count"])), data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="reports/benchmark-report.html")
    parser.add_argument("--json", default="reports/benchmark-report.json")
    parser.add_argument("--requests", type=int, default=200000)
    parser.add_argument("--clients", type=int, default=50)
    parser.add_argument("--pipelines", default="1,16,64")
    parser.add_argument("--value-size", type=int, default=16)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    bench, ddup = root / "build/ddup-bench", root / "build/ddup-server"
    valkey = shutil.which("valkey-server") or shutil.which("redis-server")
    if not bench.exists() or not ddup.exists() or not valkey:
        raise SystemExit("build binaries and valkey-server/redis-server are required")
    competitor = server_identity(valkey)
    products = [{"name": "ddup (1 worker)", "kind": "ddup", "server": ddup, "workers": 1},
                {"name": "ddup (2 workers)", "kind": "ddup", "server": ddup, "workers": 2},
                {"name": competitor, "kind": "reference", "server": valkey, "workers": 1}]
    pipelines = [int(v) for v in args.pipelines.split(",") if v]
    results = []
    for product in products:
        for pipeline in pipelines:
            for mode in ("set", "get", "ping"):
                print(product["name"], mode, "P" + str(pipeline), flush=True)
                try:
                    result = case(product, bench, args.requests, args.clients,
                                  pipeline, mode, args.value_size)
                    result["status"] = "ok"
                except RuntimeError as exc:
                    result = {"product": product["name"], "mode": mode,
                              "clients": args.clients, "pipeline": pipeline,
                              "value_size": args.value_size, "status": "failed",
                              "error": str(exc)}
                    print("FAILED:", exc, flush=True)
                results.append(result)
    payload = {"generated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
               "environment": platform.platform(),
               "cpu_count": __import__("os").cpu_count() or 0,
               "products": [p["name"] for p in products],
               "baseline": {"requests": args.requests, "clients": args.clients,
                            "pipelines": pipelines, "value_size": args.value_size},
               "results": results}
    output, json_path = root / args.output, root / args.json
    output.parent.mkdir(parents=True, exist_ok=True)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    output.write_text(render(payload), encoding="utf-8")
    print("wrote", output)
    print("wrote", json_path)


if __name__ == "__main__":
    main()
