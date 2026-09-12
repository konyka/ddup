#!/usr/bin/env python3
"""TDD checks for bounded GitHub Actions jobs."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def workflow_jobs(path):
    text = path.read_text(encoding="utf-8")
    jobs = {}
    current = None
    in_jobs = False
    for line in text.splitlines():
        if line == "jobs:":
            in_jobs = True
            continue
        if in_jobs and line and not line.startswith(" "):
            break
        if not in_jobs:
            continue
        match = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line)
        if match:
            current = match.group(1)
            jobs[current] = ""
        elif current is not None:
            jobs[current] += line + "\n"
    return jobs


def main():
    workflows = sorted((ROOT / ".github/workflows").glob("*.yml"))
    assert workflows, "no GitHub Actions workflows found"
    for path in workflows:
        jobs = workflow_jobs(path)
        assert jobs, f"{path}: jobs section must define at least one job"
        for name, body in jobs.items():
            match = re.search(r"(?m)^    timeout-minutes:\s*([1-9][0-9]*)\s*$", body)
            assert match, f"{path}: job {name} must have timeout-minutes"
            timeout = int(match.group(1))
            assert timeout <= 1440, \
                f"{path}: job {name} timeout-minutes must be <= 1440"
    print("CI timeout configuration: ok")


if __name__ == "__main__":
    main()
