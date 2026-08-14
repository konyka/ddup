#!/usr/bin/env python3
"""Redis 7 command-compat audit for ddup.

Computes the command gap between a Redis 7.x `src/commands/*.json` tree and
the ddup command table in src/core/command.c.  Command names are normalized
to lowercase; container subcommands are emitted as "<CONTAINER> <SUB>".

Outputs (--json) a stable machine-readable report containing:

  * baseline:  Redis tag, JSON file count, command-entry count;
  * ddup:      top-level command count from CMD_TABLE;
  * missing_top:      Redis top-level commands absent from CMD_TABLE;
  * missing_containers: container names with *no* implemented subcommand;
  * missing_subs:     "<CONTAINER> <SUB>" pairs missing inside implemented
                      containers.

--check additionally asserts that the gap matches the baseline block in
docs/redis-compat-audit.md (AUDIT-BASELINE-START/END HTML comment).  Any
new gap, or any gap that has been implemented but not removed from the
report, fails the check -- exactly the drift CI should catch.

The script is deliberately dependency-free (stdlib only) and read-only.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

DEFAULT_TAG = "7.2.15"
DEFAULT_REPORT = os.path.join("docs", "redis-compat-audit.md")


def norm(s):
    """Normalize case only: Redis wire-command underscores are significant."""
    return s.lower()


def load_redis_commands(json_dir, tag):
    """Return (entries, file_count) where entries maps
    normalized-name -> metadata for every Redis JSON entry.

    Top-level commands map to their name; container subcommands map to
    "CONTAINER SUB" (both normalized).
    """
    entries = {}
    files = 0
    if not os.path.isdir(json_dir):
        raise SystemExit(f"error: Redis commands dir not found: {json_dir}")
    for name in sorted(os.listdir(json_dir)):
        if not name.endswith(".json"):
            continue
        files += 1
        path = os.path.join(json_dir, name)
        try:
            with open(path, encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, ValueError) as exc:
            raise SystemExit(f"error: cannot parse {path}: {exc}")
        for cmd_name, meta in data.items():
            container = meta.get("container")
            key = norm(container + " " + cmd_name) if container else norm(cmd_name)
            if key in entries:
                raise SystemExit(f"error: duplicate command entry {key!r} in {path}")
            entries[key] = {
                "name": cmd_name,
                "container": container,
                "group": meta.get("group", ""),
                "summary": meta.get("summary", ""),
            }
    return entries, files


def read_cmd_table(command_c):
    """Return the sorted top-level command names from CMD_TABLE."""
    try:
        with open(command_c, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        raise SystemExit(f"error: cannot read {command_c}: {exc}")

    top = []
    in_table = False
    for line in text.splitlines():
        if "CMD_TABLE[]" in line:
            in_table = True
            continue
        if in_table:
            if line.startswith("};"):
                break
            m = re.search(r'\{\s*"([a-z][a-z0-9_]*)"\s*,', line)
            if m:
                top.append(m.group(1))
    return sorted(set(top))


def compute_gap(entries, top_levels, repo_root=None):
    """Return (missing_top, missing_containers, missing_subs, by_group)."""
    redis_top = sorted({k for k in entries if " " not in k})
    missing_top = sorted(set(redis_top) - set(top_levels))

    containers = defaultdict(list)
    for key, meta in entries.items():
        if meta["container"]:
            containers[norm(meta["container"])].append(key)

    implemented_containers = set()

    # Map each ci_equal(sub,...) token back to its owning container by
    # parsing the `if (cmd_id == CMD_X) { ... }` dispatch blocks with brace
    # counting, then collecting the tokens inside each block. This avoids
    # misattributing tokens shared across the big dispatch chain.
    repo_root = repo_root or os.path.join(os.path.dirname(__file__), "..")
    cmd_src = open(os.path.join(repo_root, "src", "core", "command.c"),
                   encoding="utf-8").read()
    owner = {}
    for m in re.finditer(r'if \(cmd_id == (CMD_[A-Z0-9_]+)\) \{', cmd_src):
        cid = norm(m.group(1)[4:])  # CMD_CLUSTER -> cluster
        start = cmd_src.index("{", m.start()) + 1
        depth = 1
        i = start
        while depth:
            ch = cmd_src[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            i += 1
        block = cmd_src[start:i]
        toks = set(
            norm(t)
            for t in re.findall(r'ci_equal\(\s*sub\s*,\s*[a-z0-9_]+,\s*"([A-Z0-9-]+)"\s*\)', block)
        )
        if toks:
            owner[cid] = toks
    for cname in list(containers):
        if cname in top_levels and owner.get(cname):
            implemented_containers.add(cname)

    missing_containers = []
    missing_subs = []
    for cname, keys in sorted(containers.items()):
        if cname in implemented_containers:
            have = set(owner[cname])
            for key in keys:
                sub = key.split(" ", 1)[1]
                if sub not in have:
                    missing_subs.append(key)
        else:
            missing_containers.append(cname)

    by_group = defaultdict(list)
    for key in missing_top:
        by_group[entries[key]["group"]].append(entries[key]["name"])

    return missing_top, missing_containers, missing_subs, by_group


def parse_report_baseline(report_path):
    """Parse the AUDIT-BASELINE block from the markdown report."""
    try:
        with open(report_path, encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return None
    m = re.search(r"<!-- AUDIT-BASELINE-START(.*?)AUDIT-BASELINE-END -->", text, re.S)
    if not m:
        return None
    block = m.group(1)
    missing_top = set()
    missing_containers = set()
    missing_subs = set()
    for line in block.splitlines():
        line = line.strip()
        if line.startswith("missing_top:"):
            missing_top = {norm(x) for x in line.split(":", 1)[1].split()}
        elif line.startswith("missing_containers:"):
            missing_containers = {norm(x) for x in line.split(":", 1)[1].split()}
        elif line.startswith("missing_sub:"):
            pair = line.split(":", 1)[1].strip()
            if pair:
                missing_subs.add(norm(pair))
    return {
        "missing_top": missing_top,
        "missing_containers": missing_containers,
        "missing_subs": missing_subs,
    }


def fetch_redis_commands(tag, dest):
    """Fetch src/commands/*.json for a Redis tag into dest (git sparse clone)."""
    if os.path.isdir(os.path.join(dest, "src", "commands")):
        return os.path.join(dest, "src", "commands")
    if os.path.isdir(dest) and os.listdir(dest):
        raise SystemExit(f"error: --fetch destination not empty: {dest}")
    url = f"https://github.com/redis/redis.git"
    subprocess_check(["git", "clone", "--depth", "1", "--branch", tag,
                      "--filter=blob:none", "--sparse", url, dest])
    subprocess_check(["git", "-C", dest, "sparse-checkout", "set", "src/commands"])
    return os.path.join(dest, "src", "commands")


def subprocess_check(argv):
    proc = subprocess.run(argv, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(
            f"error: {' '.join(argv)} failed:\n{proc.stdout}\n{proc.stderr}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--redis-json", metavar="DIR",
                    help="path to Redis src/commands JSON directory")
    ap.add_argument("--tag", default=DEFAULT_TAG,
                    help=f"Redis baseline tag (default {DEFAULT_TAG})")
    ap.add_argument("--fetch", metavar="DEST",
                    help="git sparse-clone Redis tag into DEST and use its commands")
    ap.add_argument("--repo", metavar="DIR", default=os.getcwd(),
                    help="ddup repo root (default: current directory)")
    ap.add_argument("--check", action="store_true",
                    help="assert gap matches docs/redis-compat-audit.md")
    ap.add_argument("--json", action="store_true", dest="emit_json",
                    help="emit machine-readable JSON (default: human text)")
    ap.add_argument("--report", default=DEFAULT_REPORT,
                    help="markdown report with AUDIT-BASELINE block")
    args = ap.parse_args()

    if args.fetch:
        json_dir = fetch_redis_commands(args.tag, args.fetch)
    elif args.redis_json:
        json_dir = args.redis_json
    else:
        raise SystemExit("error: pass --redis-json DIR or --fetch DEST")

    entries, file_count = load_redis_commands(json_dir, args.tag)
    command_c = os.path.join(args.repo, "src", "core", "command.c")
    top_levels = read_cmd_table(command_c)

    missing_top, missing_containers, missing_subs, by_group = compute_gap(
        entries, top_levels, args.repo)

    report = {
        "baseline": {
            "redis_tag": args.tag,
            "json_files": file_count,
            "entries": len(entries),
        },
        "ddup": {"cmd_table_top_level": len(top_levels)},
        "missing_top": missing_top,
        "missing_containers": missing_containers,
        "missing_subs": missing_subs,
        "by_group": {g: sorted(names) for g, names in sorted(by_group.items())},
    }

    if args.check:
        baseline = parse_report_baseline(os.path.join(args.repo, args.report))
        if baseline is None:
            raise SystemExit(
                "error: no AUDIT-BASELINE block in report; run without --check "
                "to regenerate docs/redis-compat-audit.md")
        problems = []
        extra = set(missing_top) - baseline["missing_top"]
        if extra:
            problems.append("undocumented missing commands: "
                            + " ".join(sorted(extra)))
        stale = baseline["missing_top"] - set(missing_top)
        if stale:
            problems.append("implemented but still listed as missing: "
                            + " ".join(sorted(stale)))
        missing_extra = set(missing_containers) - baseline["missing_containers"]
        if missing_extra:
            problems.append("undocumented missing containers: "
                            + " ".join(sorted(missing_extra)))
        containers_stale = baseline["missing_containers"] - set(missing_containers)
        if containers_stale:
            problems.append("implemented but still listed as missing containers: "
                            + " ".join(sorted(containers_stale)))
        subs_extra = set(missing_subs) - baseline["missing_subs"]
        if subs_extra:
            problems.append("undocumented missing subcommands: "
                            + " ".join(sorted(subs_extra)))
        subs_stale = baseline["missing_subs"] - set(missing_subs)
        if subs_stale:
            problems.append("implemented but still listed as missing subcommands: "
                            + " ".join(sorted(subs_stale)))
        if problems:
            raise SystemExit("audit FAILED\n- " + "\n- ".join(problems))
        if args.emit_json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            print("audit OK: gap matches docs/redis-compat-audit.md")
            print(f"  redis entries={len(entries)} ddup top-level={len(top_levels)}")
            print(f"  missing top={len(missing_top)} containers={len(missing_containers)} subs={len(missing_subs)}")
        return

    if args.emit_json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return

    print(f"# Redis {args.tag} vs ddup command audit")
    print(f"redis entries: {len(entries)} (json files: {file_count})")
    print(f"ddup top-level commands: {len(top_levels)}")
    print(f"missing top-level: {len(missing_top)}")
    print(f"missing containers: {len(missing_containers)}")
    print(f"missing subcommands: {len(missing_subs)}")
    print()
    print("## Missing containers")
    for c in missing_containers:
        print(f"- {c}")
    print()
    print("## Missing subcommands")
    for s in missing_subs:
        print(f"- {s}")
    print()
    print("## Missing top-level by group")
    for g, names in sorted(by_group.items()):
        print(f"### {g} ({len(names)})")
        for n in names:
            print(f"- {n}")


if __name__ == "__main__":
    main()
