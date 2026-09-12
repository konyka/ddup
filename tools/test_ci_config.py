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


def assert_token_not_traced(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        if "REPO_URL=" not in line or "GITHUB_TOKEN" not in line:
            continue
        step_start = i
        while step_start > 0 and lines[step_start].strip() != "run: |":
            step_start -= 1
        assert lines[step_start].strip() == "run: |", \
            f"{path}: tokenized shell block must use a literal run: | step"
        clone = i
        while clone < len(lines) and "git clone" not in lines[clone]:
            clone += 1
        assert clone < len(lines), f"{path}: tokenized block must clone logs"
        assert any("set +x" in lines[j] for j in range(step_start, i)), \
            f"{path}: disable shell tracing before constructing REPO_URL"


def main():
    workflow_dir = ROOT / ".github/workflows"
    workflows = sorted(set(workflow_dir.glob("*.yml")) |
                       set(workflow_dir.glob("*.yaml")))
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
        assert_token_not_traced(path)
    print("CI timeout configuration: ok")


if __name__ == "__main__":
    main()
