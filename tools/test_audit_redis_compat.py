#!/usr/bin/env python3
"""Self-tests for tools/audit_redis_compat.py (TDD).

The audit tool reads a Redis 7.x src/commands JSON tree and the ddup command
table, then --check asserts that the *current* gap set matches the documented
baseline. These tests pin the two failure modes that matter:

  1. a command that is missing today but was forgotten in the report must
     make --check fail;
  2. a command that is implemented today but still listed as missing must
     make --check fail (i.e. the report must be updated when code lands).

The fixtures below are synthetic on purpose: they let the tests run without
network access and without depending on the real Redis checkout.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUDIT = os.path.join(ROOT, "tools", "audit_redis_compat.py")


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def _fixture(tmp):
    """Create a tiny Redis-7-style commands tree + a fake ddup source tree."""
    cmds = os.path.join(tmp, "commands")
    _write(os.path.join(cmds, "get.json"), '{"GET": {"group": "string", "arity": 2}}\n')
    _write(
        os.path.join(cmds, "xadd.json"),
        '{"XADD": {"group": "stream", "arity": -5}}\n',
    )
    _write(
        os.path.join(cmds, "bitfield-ro.json"),
        '{"BITFIELD_RO": {"group": "bitmap", "arity": -2}}\n',
    )
    _write(
        os.path.join(cmds, "cluster-slots.json"),
        '{"SLOTS": {"group": "cluster", "container": "CLUSTER", "arity": 2}}\n',
    )
    _write(
        os.path.join(cmds, "cluster-links.json"),
        '{"LINKS": {"group": "cluster", "container": "CLUSTER", "arity": 2}}\n',
    )
    # ddup side: GET and CLUSTER SLOTS implemented; XADD missing; the
    # CLUSTER container exists with one subcommand.
    src = os.path.join(tmp, "src", "core")
    _write(
        os.path.join(src, "command.c"),
        "\n".join(
            [
                "static const cmd_entry CMD_TABLE[] = {",
                '    {"get", CMD_GET, 2, 2, 0, 0},',
                '    {"cluster", CMD_CLUSTER, 2, -1, 0, 0},',
                "};",
                "",
                "static void cmd_dispatch(...) {",
                "    if (cmd_id == CMD_CLUSTER) {",
                '        if (ci_equal(sub, sl, "SLOTS") && argc == 2) { return; }',
                "    }",
                "}",
                "",
            ]
        ),
    )
    _write(
        os.path.join(tmp, "docs", "redis-compat-audit.md"),
        "\n".join(
            [
                "<!-- AUDIT-BASELINE-START",
                "missing_top: bitfield_ro xadd",
                "missing_containers: ",
                "missing_sub: cluster links",
                "AUDIT-BASELINE-END -->",
                "",
                "## Missing",
                "- xadd",
                "",
            ]
        ),
    )
    return tmp


def run_audit(tmp, *extra):
    return subprocess.run(
        [sys.executable, AUDIT, "--redis-json", os.path.join(tmp, "commands")]
        + list(extra),
        capture_output=True,
        text=True,
        cwd=tmp,
    )


def test_ok_when_report_matches(tmp):
    proc = run_audit(tmp, "--check")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_fails_on_undocumented_missing(tmp):
    # Drop XADD from the report: the audit finds a missing command that the
    # baseline does not account for, so --check must fail.
    report = os.path.join(tmp, "docs", "redis-compat-audit.md")
    with open(report, encoding="utf-8") as fh:
        text = fh.read()
    text = text.replace("missing_top: bitfield_ro xadd", "missing_top: bitfield_ro")
    text = text.replace("- xadd\n", "")
    _write(report, text)
    proc = run_audit(tmp, "--check")
    assert proc.returncode != 0, "undocumented missing command must fail --check"
    assert "xadd" in proc.stdout + proc.stderr


def test_fails_on_stale_report_entry(tmp):
    # Keep GET listed as missing even though the fake CMD_TABLE has it.
    # --check must fail and point at GET.
    report = os.path.join(tmp, "docs", "redis-compat-audit.md")
    with open(report, encoding="utf-8") as fh:
        text = fh.read()
    text = text.replace("missing_top: bitfield_ro xadd", "missing_top: bitfield_ro get xadd")
    text = text.replace("## Missing\n- xadd\n", "## Missing\n- get\n- xadd\n")
    _write(report, text)
    proc = run_audit(tmp, "--check")
    assert proc.returncode != 0, "stale report entry must fail --check"
    assert "get" in proc.stdout + proc.stderr


def test_underscore_command_name_preserved(tmp):
    proc = run_audit(tmp, "--json")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    import json as _json
    assert "bitfield_ro" in _json.loads(proc.stdout)["missing_top"]


def test_missing_container_reported(tmp):
    # The real Redis JSON carries a container with no subcommands implemented
    # in ddup; verify the tool classifies it as an entirely missing container.
    cmds = os.path.join(tmp, "commands")
    _write(
        os.path.join(cmds, "memory-stats.json"),
        '{"STATS": {"group": "server", "container": "MEMORY", "arity": 2}}\n',
    )
    import json as _json
    proc = run_audit(tmp, "--json")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    data = _json.loads(proc.stdout)
    assert data["missing_containers"] == ["memory"]
    assert data["missing_subs"] == ["cluster links"]  # CLUSTER SLOTS implemented


def test_fails_on_stale_container_entry(tmp):
    # Add a MEMORY subcommand and implementation while the report still says
    # the whole MEMORY container is missing. --check must reject that stale
    # container-level entry.
    cmds = os.path.join(tmp, "commands")
    _write(
        os.path.join(cmds, "memory-stats.json"),
        '{"STATS": {"group": "server", "container": "MEMORY", "arity": 2}}\n',
    )
    cmd_c = os.path.join(tmp, "src", "core", "command.c")
    with open(cmd_c, encoding="utf-8") as fh:
        text = fh.read()
    text = text.replace('    {"cluster", CMD_CLUSTER, 2, -1, 0, 0},',
                        '    {"cluster", CMD_CLUSTER, 2, -1, 0, 0},\n'
                        '    {"memory", CMD_MEMORY, 2, -1, 0, 0},')
    text = text.replace('    }\n}',
                        '    }\n    if (cmd_id == CMD_MEMORY) {\n'
                        '        if (ci_equal(sub, sl, "STATS")) { return; }\n'
                        '    }\n}')
    _write(cmd_c, text)
    report = os.path.join(tmp, "docs", "redis-compat-audit.md")
    with open(report, encoding="utf-8") as fh:
        report_text = fh.read()
    report_text = report_text.replace("missing_containers: ",
                                      "missing_containers: memory")
    _write(report, report_text)
    proc = run_audit(tmp, "--check")
    assert proc.returncode != 0, "stale container entry must fail --check"
    assert "memory" in proc.stdout + proc.stderr


def test_fails_on_stale_subcommand_entry(tmp):
    # Pretend CLUSTER LINKS is implemented: the report still lists it as
    # missing, so --check must fail.
    cmd_c = os.path.join(tmp, "src", "core", "command.c")
    with open(cmd_c, encoding="utf-8") as fh:
        text = fh.read()
    text = text.replace(
        'if (ci_equal(sub, sl, "SLOTS") && argc == 2) { return; }',
        'if (ci_equal(sub, sl, "SLOTS") && argc == 2) { return; }\n'
        '        if (ci_equal(sub, sl, "LINKS") && argc == 2) { return; }',
    )
    _write(cmd_c, text)
    proc = run_audit(tmp, "--check")
    assert proc.returncode != 0, "stale subcommand entry must fail --check"
    assert "cluster links" in proc.stdout + proc.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--keep", action="store_true", help="keep tmp dirs on failure")
    args = parser.parse_args()
    tmp = tempfile.mkdtemp(prefix="audit-test-")
    failures = 0
    try:
        for fn in (
            test_ok_when_report_matches,
            test_fails_on_undocumented_missing,
            test_fails_on_stale_report_entry,
            test_underscore_command_name_preserved,
            test_missing_container_reported,
            test_fails_on_stale_container_entry,
            test_fails_on_stale_subcommand_entry,
        ):
            fixture = tempfile.mkdtemp(prefix="audit-test-")
            try:
                fn(_fixture(fixture))
                print(f"{fn.__name__}: ok")
            except AssertionError as exc:
                failures += 1
                print(f"{fn.__name__}: FAILED: {exc}")
            finally:
                shutil.rmtree(fixture, ignore_errors=True)
        print(f"---\n{7 - failures}/7 audit tool tests passed")
    finally:
        if failures and not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
